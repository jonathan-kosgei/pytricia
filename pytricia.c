#include <arpa/inet.h>
#include <Python.h>
#include "patricia.h"

typedef struct {
    PyObject_HEAD
    patricia_tree_t *m_tree;
} PyTricia;

typedef struct {
    PyObject_HEAD
    patricia_tree_t *m_tree;
    patricia_node_t *m_Xnode;
    patricia_node_t *m_Xhead;
    patricia_node_t **m_Xstack;
    patricia_node_t **m_Xsp;
    patricia_node_t *m_Xrn;
    PyTricia *m_parent;
} PyTriciaIter;

static const int ADDRSTRLEN = 32;

static void *inet_ntop_with_prefix(int family, const void *src, char *dst, int bufflen) {
    if (inet_ntop(family, src, dst, bufflen) == NULL) {
        return NULL;
    }
    strncat(dst, "/32", bufflen);
    return dst;
}


/*
 * Convert a Pytricia key to a string representation that can be converted,
 * eventually, to a to prefix_t for use in the Pytricia structure.
 * return 0 on success, -1 on failure.
 */
static int convert_key_to_cstring(PyObject* key, char keystr[ADDRSTRLEN]) {
    memset(keystr, 0, ADDRSTRLEN); 

#if PY_MAJOR_VERSION >= 3
    if (PyUnicode_Check(key)) {
        int rv = PyUnicode_READY(key); 
        if (rv < 0) { 
            return -1;
        }
        char* temp = PyUnicode_AsUTF8(key);
        if (temp == NULL) {
            return -1;
        }
        strncpy(keystr, temp, ADDRSTRLEN);
        return 0;
    } else if (PyLong_Check(key)) {
        long val = htonl(PyLong_AsLong(key));
        return (inet_ntop_with_prefix(AF_INET, &val, keystr, ADDRSTRLEN) == NULL);
    } 
#if PY_MINOR_VERSION >= 4
    // do we have an IPv4Address or IPv4Network object (ipaddress
    // module added in Python 3.4


#endif

#else // Python v2
    if (PyString_Check(key)) {
        char* temp = PyString_AsString(key);
        strncpy(keystr, temp, ADDRSTRLEN);
        return 0;
    } else if (PyInt_Check(key)) {
        long val = htonl(PyInt_AsLong(key));
        return (inet_ntop_with_prefix(AF_INET, &val, keystr, ADDRSTRLEN) == NULL);
    }
#endif
    return -1;
}


static void
pytricia_dealloc(PyTricia* self)
{
    if (self) {
        Destroy_Patricia(self->m_tree, 0);
        Py_TYPE(self)->tp_free((PyObject*)self);
    }
}

static PyObject *
pytricia_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyTricia *self;

    self = (PyTricia*)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->m_tree = NULL;
    }
    return (PyObject *)self;
}

static int
pytricia_init(PyTricia *self, PyObject *args, PyObject *kwds)
{
    int prefixlen = 32;
    if (!PyArg_ParseTuple(args, "|i", &prefixlen)) {
        self->m_tree = New_Patricia(1); // need to have *something* to dealloc
        PyErr_SetString(PyExc_ValueError, "Error parsing prefix length.");
        return -1;
    }

    if (prefixlen < 0 || prefixlen > PATRICIA_MAXBITS) {
        self->m_tree = New_Patricia(1); // need to have *something* to dealloc
        PyErr_SetString(PyExc_ValueError, "Invalid number of maximum bits; must be between 0 and 128, inclusive.");
        return -1;
    } 
    
    self->m_tree = New_Patricia(prefixlen);
    if (self->m_tree == NULL) {
        return -1;
    }
    return 0;
}

static Py_ssize_t 
pytricia_length(PyTricia *self) 
{
    patricia_node_t *node = NULL;
    Py_ssize_t count = 0;

    PATRICIA_WALK (self->m_tree->head, node) {
        count += 1;
    } PATRICIA_WALK_END;
    return count;
}

/* from SubnetTree.cc */
/* -- TODO: make compatible with IPv6 */
static prefix_t* 
make_prefix(unsigned long addr, int width)
{
    prefix_t* subnet = (prefix_t*)malloc(sizeof(prefix_t));
    if (subnet == NULL) {
        return NULL;
    }

    memcpy(&subnet->add.sin, &addr, sizeof(subnet->add.sin)) ;
    subnet->family = AF_INET;
    subnet->bitlen = width;
    subnet->ref_count = 1;

    return subnet;
}

/* -- TODO: make compatible with IPv6 -- */
/* 
   -- problem: buffer only designed for IPv4 --
*/
static int 
parse_cidr(const char *cidr, unsigned long *subnet, unsigned short *masklen)
{
    static char buffer[32];
    struct in_addr addr;

    if (!cidr) {
        return -1;
    }

    const char *s = strchr(cidr, '/'); // find the address before the prefix

    // if found
    if (s) {
        unsigned long len = s - cidr < 32 ? s - cidr : 31;
        memcpy(buffer, cidr, len);
        buffer[len] = '\0';
        *masklen = atoi(s+1);
        s = buffer;
    } else { // not found; assume all significant (255.255.255.255)
        s = cidr;
        *masklen = 32;
    }

    if (!inet_aton((char *)(s), &addr)) {
        return -1;
    }

    *subnet = addr.s_addr;
    return 0;
}

static prefix_t*
pystr_to_prefix(char* str) // Changed to take in a string instead of a pystr.
{
    if (!str) { // If str isn't populated for some reason -> come back to this.
        return NULL;
    }

    unsigned long subnet = 0UL;
    unsigned short mask = 0;
    if (parse_cidr(str, &subnet, &mask)) {
        return NULL;
    }
    
    return make_prefix(subnet, mask);
}

static PyObject* 
pytricia_subscript(PyTricia *self, PyObject *key)
{
    char keystr[ADDRSTRLEN];
    int rv = convert_key_to_cstring(key, keystr); 
    if (rv < 0) {
        PyErr_SetString(PyExc_ValueError, "Error parsing key.");
        return NULL;
    }

    prefix_t* subnet = pystr_to_prefix(keystr);
    if (subnet == NULL) {
        PyErr_SetString(PyExc_ValueError, "Error parsing prefix.");
        return NULL;
    }
    
    patricia_node_t* node = patricia_search_best(self->m_tree, subnet);
    Deref_Prefix(subnet);

    if (!node) {
        PyErr_SetString(PyExc_KeyError, "Prefix not found.");
        return NULL;
    }

    PyObject* data = (PyObject*)node->data;

    Py_INCREF(data);
    return data;
}

static int
pytricia_internal_delete(PyTricia *self, PyObject *key)
{
    char keystr[ADDRSTRLEN];
    int rv = convert_key_to_cstring(key, keystr); 
    if (rv < 0) {
        PyErr_SetString(PyExc_ValueError, "Error parsing key.");
        return -1;
    }


    prefix_t* prefix = pystr_to_prefix(keystr);
    if (prefix == NULL) {
        PyErr_SetString(PyExc_ValueError, "Error parsing prefix.");
        return -1;
    }

    patricia_node_t* node = patricia_search_exact(self->m_tree, prefix);
    Deref_Prefix(prefix);

    if (!node) {
        PyErr_SetString(PyExc_KeyError, "Prefix doesn't exist.");
        return -1;
    }

    // decrement ref count on data referred to by key, if it exists
    PyObject* data = (PyObject*)node->data;
    Py_XDECREF(data);

    patricia_remove(self->m_tree, node);
    return 0;
}

static int 
pytricia_assign_subscript(PyTricia *self, PyObject *key, PyObject *value)
{
    if (!value) {
        return pytricia_internal_delete(self, key);
    }
    
    char keystr[ADDRSTRLEN];
    int rv = convert_key_to_cstring(key, keystr); 
    if (rv < 0) {
        PyErr_SetString(PyExc_ValueError, "Error parsing key.");
        return -1;
    }
    
    patricia_node_t* node = make_and_lookup(self->m_tree, keystr);
    if (!node) {
        PyErr_SetString(PyExc_ValueError, "Error inserting into patricia tree");
        return -1;
    }

    Py_INCREF(value);
    node->data = value;

    return 0;
}

static PyObject*
pytricia_insert(PyTricia *self, PyObject *args) {
    PyObject *key = NULL;
    PyObject *value = NULL;

    if (!PyArg_ParseTuple(args, "OO", &key, &value)) { 
        return NULL;
    }

    int rv = pytricia_assign_subscript(self, key, value);
    if (rv == -1) {
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject*
pytricia_delitem(PyTricia *self, PyObject *args)
{
    PyObject *key = NULL;
    if (!PyArg_ParseTuple(args, "O", &key)) {
        PyErr_SetString(PyExc_ValueError, "Unclear what happened!");
        return NULL;
    }
    
    int rv = pytricia_internal_delete(self, key);
    if (rv < 0) {
        return NULL;
    } 
    Py_RETURN_NONE;
}

static PyObject *
pytricia_get(register PyTricia *obj, PyObject *args)
{
    PyObject *key = NULL;
    PyObject *defvalue = NULL;

    if (!PyArg_ParseTuple(args, "O|O:get", &key, &defvalue)) {
        return NULL;
    }

    char keystr[ADDRSTRLEN];
    int rv = convert_key_to_cstring(key, keystr); 
    if (rv < 0) {
        PyErr_SetString(PyExc_ValueError, "Error parsing key.");
        return NULL;
    }

    prefix_t* prefix = pystr_to_prefix(keystr);
    if (prefix == NULL) {
        PyErr_SetString(PyExc_ValueError, "Error parsing prefix.");
        return NULL;
    }
    
    patricia_node_t* node = patricia_search_best(obj->m_tree, prefix);
    Deref_Prefix(prefix);

    if (!node) {
        if (defvalue) {
            Py_INCREF(defvalue);
            return defvalue;
        }
        Py_RETURN_NONE;
    }

    PyObject* data = (PyObject*)node->data;
    Py_INCREF(data);
    return data;
}

static int
pytricia_contains(PyTricia *self, PyObject *key)
{
    char keystr[ADDRSTRLEN];
    int rv = convert_key_to_cstring(key, keystr); 
    if (rv < 0) {
        PyErr_SetString(PyExc_ValueError, "Error parsing key.");
        return -1;
    }

    prefix_t* prefix = pystr_to_prefix(keystr);
    if (!prefix) {
        PyErr_SetString(PyExc_ValueError, "Error parsing prefix.");
        return -1;
    }
    
    patricia_node_t* node = patricia_search_best(self->m_tree, prefix);
    Deref_Prefix(prefix);
    if (node) {
        return 1;
    }
    return 0;
}

static PyObject*
pytricia_has_key(PyTricia *self, PyObject *args)
{
    PyObject *key = NULL;
    if (!PyArg_ParseTuple(args, "O", &key))
        return NULL;
        
    char keystr[ADDRSTRLEN];
    int rv = convert_key_to_cstring(key, keystr); 
    if (rv < 0) {
        PyErr_SetString(PyExc_ValueError, "Error parsing key.");
        return NULL;
    }

    prefix_t* prefix = pystr_to_prefix(keystr);
    if (!prefix) {
        PyErr_SetString(PyExc_ValueError, "Error parsing prefix.");
        return NULL;
    }
    
    patricia_node_t* node = patricia_search_exact(self->m_tree, prefix);
    Deref_Prefix(prefix);
    if (node) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject* 
pytricia_keys(register PyTricia *self, PyObject *unused)
{
    register PyObject *rvlist = PyList_New(0);
    if (!rvlist) {
        return NULL;
    }
    
    patricia_node_t *node = NULL;
    int err = 0;
    
    PATRICIA_WALK (self->m_tree->head, node) {
        char buffer[64];
        prefix_toa2x(node->prefix, buffer, 1);
        PyObject *item = Py_BuildValue("s", buffer);
        if (!item) {
            Py_DECREF(rvlist);
            return NULL;
        }
        err = PyList_Append(rvlist, item);
        Py_INCREF(item);
        if (err != 0) {
            Py_DECREF(rvlist);
            return NULL;
        }
    } PATRICIA_WALK_END;
    return rvlist;
}

static PyMappingMethods pytricia_as_mapping = {
    (lenfunc)pytricia_length,
    (binaryfunc)pytricia_subscript,
    (objobjargproc)pytricia_assign_subscript
};

static PySequenceMethods pytricia_as_sequence = {
       (lenfunc)pytricia_length,               /*sq_length*/
       0,              /*sq_concat*/
       0,                      /*sq_repeat*/
       0,                      /*sq_item*/
       0,                      /*sq_slice*/
       0,                      /*sq_ass_item*/
       0,                  /*sq_ass_slice*/
       (objobjproc)pytricia_contains,               /*sq_contains*/
       0,                  /*sq_inplace_concat*/
       0                   /*sq_inplace_repeat*/
};

// forward declaration
static PyObject*
pytricia_iter(register PyTricia *, PyObject *);


static PyMethodDef pytricia_methods[] = {
    {"has_key",   (PyCFunction)pytricia_has_key, METH_VARARGS, "has_key(prefix) -> boolean\nReturn true iff prefix is in tree.  Note that this method checks for an *exact* match with the prefix.\nUse the 'in' operator if you want to test whether a given address is 'valid' in the tree."},
    {"keys",   (PyCFunction)pytricia_keys, METH_NOARGS, "keys() -> list\nReturn a list of all prefixes in the tree."},
    {"get", (PyCFunction)pytricia_get, METH_VARARGS, "get(prefix, [default]) -> object\nReturn value associated with prefix."},
    {"delete", (PyCFunction)pytricia_delitem, METH_VARARGS, "delete(prefix) -> \nDelete mapping associated with prefix.\n"},
    {"insert", (PyCFunction)pytricia_insert, METH_VARARGS, "insert(prefix, data) -> data\nCreate mapping between prefix and data in tree."},
    {NULL,              NULL}           /* sentinel */
};

static PyObject*
pytriciaiter_iter(PyTricia *self)
{
    Py_INCREF(self);
    return (PyObject*)self;
}

static PyObject*
pytriciaiter_next(PyTriciaIter *iter)
{
    while (1) {
        iter->m_Xnode = iter->m_Xrn;
        if (iter->m_Xnode) {
            // advance the iterator; copied from patricia.h macros
            if (iter->m_Xrn->l) { 
                if (iter->m_Xrn->r) { 
                    *(iter->m_Xsp)++ = iter->m_Xrn->r; 
                } 
                iter->m_Xrn = iter->m_Xrn->l; 
            } else if (iter->m_Xrn->r) { 
                iter->m_Xrn = iter->m_Xrn->r; 
            } else if (iter->m_Xsp != iter->m_Xstack) { 
                iter->m_Xrn = *(--iter->m_Xsp); 
            } else { 
                iter->m_Xrn = (patricia_node_t *) 0; 
            } 

            if (iter->m_Xnode->prefix) {
                /* build Python value to hand back */
                char buffer[64];
                prefix_toa2x(iter->m_Xnode->prefix, buffer, 1);
                PyObject *rv = Py_BuildValue("s", buffer);
                return rv;
            } 
        } else {
            PyErr_SetNone(PyExc_StopIteration);
            return NULL;
        }
    }
    // should NEVER get here...
    assert(NULL);
    return NULL;
}

static void
pytriciaiter_dealloc(PyTriciaIter *iterobj)
{
    if (iterobj->m_Xstack) {
        free(iterobj->m_Xstack);
    }
    Py_DECREF(iterobj->m_parent);
    Py_TYPE(iterobj)->tp_free((PyObject*)iterobj);    
}

static PyTypeObject PyTriciaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pytricia.PyTricia",       /*tp_name*/
    sizeof(PyTricia),          /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)pytricia_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    &pytricia_as_sequence,     /*tp_as_sequence*/
    &pytricia_as_mapping,      /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "PyTricia objects",        /* tp_doc */
    0,		               /* tp_traverse */
    0,		               /* tp_clear */
    0,		               /* tp_richcompare */
    0,		               /* tp_weaklistoffset */
    (getiterfunc)pytricia_iter,	/* tp_iter */
    0,		               /* tp_iternext */
    pytricia_methods,          /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)pytricia_init,   /* tp_init */
    0,                         /* tp_alloc */
    pytricia_new,              /* tp_new */
};

static PyTypeObject PyTriciaIterType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pytricia.PyTriciaIter",                /* tp_name */
    sizeof(PyTriciaIter),                   /* tp_basicsize */
    0,                                      /* tp_itemsize */
    /* methods */
    (destructor)pytriciaiter_dealloc,       /* tp_dealloc */
    0,                                      /* tp_print */
    0,                                      /* tp_getattr */
    0,                                      /* tp_setattr */
    0,                                      /* tp_compare */
    0,                                      /* tp_repr */
    0,                                      /* tp_as_number */
    0,                                      /* tp_as_sequence */
    0,                                      /* tp_as_mapping */
    0,                                      /* tp_hash */
    0,                                      /* tp_call */
    0,                                      /* tp_str */
    0,                                      /* tp_getattro */
    0,                                      /* tp_setattro */
    0,                                      /* tp_as_buffer */
#if PY_MAJOR_VERSION >= 3
    Py_TPFLAGS_DEFAULT,                     /* tp_flags */
#else
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_ITER, /* tp_flags */
#endif
    "Internal PyTricia iter object",        /* tp_doc */
    0,                                      /* tp_traverse */
    0,                                      /* tp_clear */
    0,                                      /* tp_richcompare */
    0,                                      /* tp_weaklistoffset */
    (getiterfunc)pytriciaiter_iter,         /* tp_iter */
    (iternextfunc)pytriciaiter_next,        /* tp_iternext */
    0,                                      /* tp_methods */
};

static PyObject*
pytricia_iter(register PyTricia *self, PyObject *unused)
{
    PyTriciaIter *iterobj = PyObject_New(PyTriciaIter, &PyTriciaIterType);
    if (!iterobj) {
        return NULL;
    }

    if (!PyObject_Init((PyObject*)iterobj, &PyTriciaIterType)) {
        Py_XDECREF(iterobj);
        return NULL;
    }

    Py_INCREF(self);
    iterobj->m_parent = self;

    iterobj->m_tree = self->m_tree;
    iterobj->m_Xnode = NULL;
    iterobj->m_Xhead = iterobj->m_tree->head;
    iterobj->m_Xstack = (patricia_node_t**) malloc(sizeof(patricia_node_t*)*(PATRICIA_MAXBITS+1));
    if (!iterobj->m_Xstack) {
        Py_DECREF(iterobj->m_parent);
        Py_TYPE(iterobj)->tp_free((PyObject*)iterobj);
        return PyErr_NoMemory();
    }
 
    iterobj->m_Xsp = iterobj->m_Xstack;
    iterobj->m_Xrn = iterobj->m_Xhead;
    return (PyObject*)iterobj;
}


PyDoc_STRVAR(pytricia_doc,
"Yet another patricia tree module in Python.  But this one's better.\n\
");

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef pytricia_moduledef = {
    PyModuleDef_HEAD_INIT,
    "pytricia",       /* m_name */
    pytricia_doc,     /* m_doc */
    -1,               /* m_size */
    NULL,             /* m_methods */
    NULL,             /* m_reload */
    NULL,             /* m_traverse */
    NULL,             /* m_clear */
    NULL,             /* m_free */
};
#endif

#ifndef PyMODINIT_FUNC	/* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif

#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC
PyInit_pytricia(void) 
#else
PyMODINIT_FUNC
initpytricia(void) 
#endif
{
    PyObject* m;

    if (PyType_Ready(&PyTriciaType) < 0)
#if PY_MAJOR_VERSION >= 3
        return NULL;
#else
        return;
#endif

    PyTriciaIterType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&PyTriciaIterType) < 0)
#if PY_MAJOR_VERSION >= 3
        return NULL;
#else
        return;
#endif

#if PY_MAJOR_VERSION >= 3
    m = PyModule_Create(&pytricia_moduledef);
#else
    m = Py_InitModule3("pytricia", NULL, pytricia_doc);
#endif
    if (m == NULL)
#if PY_MAJOR_VERSION >= 3 
      return NULL;
#else
      return;
#endif

    Py_INCREF(&PyTriciaType);
    Py_INCREF(&PyTriciaIterType);
    PyModule_AddObject(m, "PyTricia", (PyObject *)&PyTriciaType);

    // JS: don't add the PyTriciaIter object to the public interface.  users shouldn't be
    // able to create iterator objects w/o calling __iter__ on a pytricia object.

#if PY_MAJOR_VERSION >= 3
    return m;
#endif
}

