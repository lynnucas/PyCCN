#include <Python.h>
#include <ccn/ccn.h>
#include <ccn/hashtb.h>
#include <ccn/reg_mgmt.h>
#include <ccn/signing.h>

#include "pyccn.h"
#include "key_utils.h"
#include "methods_contentobject.h"
#include "methods_interest.h"
#include "methods_key.h"
#include "methods_name.h"
#include "methods_signature.h"
#include "methods_signedinfo.h"
#include "misc.h"
#include "objects.h"

// *** Python method declarations
//
//
// ** Methods of CCN
//
// Daemon
//
// arguments: none
// returns:  CObject that is an opaque reference to the ccn handle

static PyObject * // CCN
_pyccn_ccn_create(PyObject *UNUSED(self), PyObject *UNUSED(args))
{
	struct ccn *ccn_handle = ccn_create();

	if (!ccn_handle) {
		PyErr_SetString(g_PyExc_CCNError,
				"ccn_create() failed for an unknown reason"
				" (out of memory?).");
		return NULL;
	}

	return CCNObject_New(HANDLE, ccn_handle);
}

// Second argument to ccn_connect not yet supported
//
// arguments:  CObject that is an opaque reference to the ccn handle, generated by _pyccn_ccn_create
// returns:    integer, non-negative if ok (file descriptor)
//

static PyObject *
_pyccn_ccn_connect(PyObject *UNUSED(self), PyObject *py_ccn_handle)
{
	struct ccn *handle;
	int r;

	if (!CCNObject_IsValid(HANDLE, py_ccn_handle)) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN Handle");
		return NULL;
	}
	handle = CCNObject_Get(HANDLE, py_ccn_handle);

	r = ccn_connect(handle, NULL);
	if (r < 0) {
		int err = ccn_geterror(handle);
		return PyErr_Format(g_PyExc_CCNError, "Unable to connect with"
				" CCN daemon: %s [%d]", strerror(err), err);
	}

	return Py_BuildValue("i", r);
}

// arguments:  CObject that is an opaque reference to the ccn handle, generated by _pyccn_ccn_create
// returns: None
//

static PyObject *
_pyccn_ccn_disconnect(PyObject *UNUSED(self), PyObject *py_ccn_handle)
{
	struct ccn *handle;
	int r;

	if (!CCNObject_IsValid(HANDLE, py_ccn_handle)) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN Handle");
		return NULL;
	}
	handle = CCNObject_Get(HANDLE, py_ccn_handle);

	r = ccn_disconnect(handle);
	if (r < 0) {
		int err = ccn_geterror(handle);
		return PyErr_Format(g_PyExc_CCNError, "Unable to disconnect"
				" with CCN daemon: %s [%d]", strerror(err), err);
	}

	Py_RETURN_NONE;
}

static PyObject *
_pyccn_ccn_run(PyObject *UNUSED(self), PyObject *args)
{
	int r;
	PyObject *py_handle;
	int timeoutms = -1;
	struct ccn *handle;

	if (!PyArg_ParseTuple(args, "O|i:_pyccn_ccn_run", &py_handle, &timeoutms))
		return NULL;

	if (!CCNObject_IsValid(HANDLE, py_handle)) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN Handle");
		return NULL;
	}
	handle = CCNObject_Get(HANDLE, py_handle);

	if (_pyccn_thread_state) {
		PyErr_SetString(g_PyExc_CCNError, "You're allowed to run ccn_run only"
				" once");
		return NULL;
	}

	/* Enable threads */
	_pyccn_thread_state = PyEval_SaveThread();

	debug("Entering ccn_run()\n");
	r = ccn_run(handle, timeoutms);
	debug("Exiting ccn_run()\n");

	/* disable threads */
	assert(_pyccn_thread_state);
	PyEval_RestoreThread(_pyccn_thread_state);
	_pyccn_thread_state = NULL;

	/*
		CCNObject_Purge_Closures();
	 */

	if (r < 0) {
		int err = ccn_geterror(handle);
		if (err == 0)
			return PyErr_Format(g_PyExc_CCNError, "ccn_run() failed"
				" for an unknown reason (possibly you're not"
				" connected to the daemon)");
		return PyErr_Format(g_PyExc_CCNError, "ccn_run() failed: %s"
				" [%d]", strerror(err), err);
	}

	Py_RETURN_NONE;
}

static PyObject *
_pyccn_ccn_set_run_timeout(PyObject *UNUSED(self), PyObject *args)
{
	int r;
	PyObject *py_handle;
	int timeoutms = 0;
	struct ccn *handle;

	if (!PyArg_ParseTuple(args, "O|i:_pyccn_ccn_set_run_timeout", &py_handle,
			&timeoutms))
		return NULL;

	if (!CCNObject_IsValid(HANDLE, py_handle)) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN handle");
		return NULL;
	}
	handle = CCNObject_Get(HANDLE, py_handle);

	r = ccn_set_run_timeout(handle, timeoutms);

	return Py_BuildValue("i", r);
}

// ************
// UpcallInfo
//
//

// Can be called directly from c library

static PyObject *
obj_UpcallInfo_from_ccn(enum ccn_upcall_kind upcall_kind,
		struct ccn_upcall_info *ui)
{
	PyObject *py_obj_UpcallInfo;
	PyObject *py_o;
	PyObject *py_data = NULL, *py_pco = NULL, *py_comps = NULL;
	struct ccn_charbuf *data;
	struct ccn_parsed_ContentObject *pco;
	struct ccn_indexbuf *comps;
	int r;

	assert(g_type_UpcallInfo);

	// Create name object
	py_obj_UpcallInfo = PyObject_CallObject(g_type_UpcallInfo, NULL);
	JUMP_IF_NULL(py_obj_UpcallInfo, error);

	// CCN handle, I hope it isn't freed; if it is freed we'll get a crash :/
	py_o = CCNObject_Borrow(HANDLE, ui->h);
	r = PyObject_SetAttrString(py_obj_UpcallInfo, "ccn", py_o);
	Py_DECREF(py_o);
	JUMP_IF_NEG(r, error);

	py_o = PyInt_FromLong(ui->matched_comps);
	r = PyObject_SetAttrString(py_obj_UpcallInfo, "matchedComps", py_o);
	Py_DECREF(py_o);
	JUMP_IF_NULL(py_o, error);

	if (upcall_kind == CCN_UPCALL_CONTENT ||
			upcall_kind == CCN_UPCALL_CONTENT_UNVERIFIED ||
			upcall_kind == CCN_UPCALL_CONTENT_BAD) {

		py_data = CCNObject_New_charbuf(CONTENT_OBJECT, &data);
		JUMP_IF_NULL(py_data, error);
		r = ccn_charbuf_append(data, ui->content_ccnb,
				ui->pco->offset[CCN_PCO_E]);
		JUMP_IF_NEG_MEM(r, error);

		py_pco = CCNObject_New_ParsedContentObject(&pco);
		JUMP_IF_NULL(py_pco, error);

		py_comps = CCNObject_New_ContentObjectComponents(&comps);
		JUMP_IF_NULL(py_comps, error);

		r = ccn_parse_ContentObject(data->buf, data->length,
				pco, comps);
		if (r < 0) {
			PyErr_Format(g_PyExc_CCNError, "Unable to generate Upcall:"
					" ccn_parse_ContentObject returned %d", r);
			goto error;
		}

		py_o = ContentObject_from_ccn_parsed(py_data, py_pco, py_comps);
		Py_CLEAR(py_comps);
		Py_CLEAR(py_pco);
		Py_CLEAR(py_data);
		JUMP_IF_NULL(py_o, error);

		r = PyObject_SetAttrString(py_obj_UpcallInfo, "ContentObject", py_o);
		Py_DECREF(py_o);
		JUMP_IF_NEG(r, error);
	}

	if (upcall_kind == CCN_UPCALL_INTEREST ||
			upcall_kind == CCN_UPCALL_CONSUMED_INTEREST) {

		py_data = CCNObject_New_charbuf(INTEREST, &data);
		JUMP_IF_NULL(py_data, error);
		r = ccn_charbuf_append(data, ui->interest_ccnb,
				ui->pi->offset[CCN_PI_E]);
		JUMP_IF_NEG_MEM(r, error);

		py_o = obj_Interest_from_ccn(py_data);
		Py_CLEAR(py_data);
		JUMP_IF_NULL(py_o, error);

		r = PyObject_SetAttrString(py_obj_UpcallInfo, "Interest", py_o);
		Py_DECREF(py_o);
		JUMP_IF_NEG(r, error);
	}

	return py_obj_UpcallInfo;

error:
	Py_XDECREF(py_comps);
	Py_XDECREF(py_pco);
	Py_XDECREF(py_data);
	Py_XDECREF(py_obj_UpcallInfo);
	return NULL;
}

static enum ccn_upcall_res
ccn_upcall_handler(struct ccn_closure *selfp,
		enum ccn_upcall_kind upcall_kind,
		struct ccn_upcall_info *info)
{
	PyObject *upcall_method = NULL, *py_upcall_info = NULL;
	PyObject *py_selfp, *py_closure, *arglist, *result;

	debug("upcall_handler dispatched kind %d\n", upcall_kind);

	assert(selfp);
	assert(selfp->data);

	//XXX: What to do when ccn_run is not called?
	if (!_pyccn_thread_state) {
		debug("ccn_run() is not currently running\n");
		return CCN_UPCALL_RESULT_ERR;
	}

	/* acquiring lock */
	assert(_pyccn_thread_state);
	PyEval_RestoreThread(_pyccn_thread_state);

	/* equivalent of selfp, wrapped into PyCapsule */
	py_selfp = selfp->data;
	py_closure = PyCapsule_GetContext(py_selfp);
	assert(py_closure);

	upcall_method = PyObject_GetAttrString(py_closure, "upcall");
	JUMP_IF_NULL(upcall_method, error);

	debug("Generating UpcallInfo\n");
	py_upcall_info = obj_UpcallInfo_from_ccn(upcall_kind, info);
	JUMP_IF_NULL(py_upcall_info, error);
	debug("Done generating UpcallInfo\n");

	arglist = Py_BuildValue("iO", upcall_kind, py_upcall_info);
	Py_CLEAR(py_upcall_info);

	debug("Calling upcall\n");

	result = PyObject_CallObject(upcall_method, arglist);
	Py_CLEAR(upcall_method);
	Py_DECREF(arglist);
	JUMP_IF_NULL(result, error);

	if (upcall_kind == CCN_UPCALL_FINAL)
		Py_DECREF(py_selfp);
	/*
		CCNObject_Complete_Closure(py_selfp);
	 */

	long r = PyInt_AsLong(result);

	/* releasing lock */
	_pyccn_thread_state = PyEval_SaveThread();

	return r;

error:
	debug("Error routine called (upcall_kind = %d)\n", upcall_kind);
	/*
		CCNObject_Complete_Closure(py_selfp);
	 */
	if (upcall_kind == CCN_UPCALL_FINAL)
		Py_DECREF(py_selfp);
	Py_XDECREF(py_upcall_info);
	Py_XDECREF(upcall_method);

	//XXX: What to do with the exceptions thrown?
	if (PyErr_Occurred()) {
		PyErr_Print();
	}

	_pyccn_thread_state = PyEval_SaveThread();
	return CCN_UPCALL_RESULT_ERR;
}

// Registering callbacks

static PyObject *
_pyccn_ccn_express_interest(PyObject *UNUSED(self), PyObject *args)
{
	PyObject *py_o, *py_ccn, *py_name, *py_closure, *py_templ;
	int r;
	struct ccn *handle;
	struct ccn_charbuf *name, *templ;
	struct ccn_closure *cl;

	if (!PyArg_ParseTuple(args, "OOOO", &py_ccn, &py_name, &py_closure,
			&py_templ))
		return NULL;

	if (strcmp(py_ccn->ob_type->tp_name, "CCN") != 0) {
		PyErr_SetString(PyExc_TypeError, "Must pass a ccn as arg 1");
		return NULL;
	}
	if (strcmp(py_name->ob_type->tp_name, "Name") != 0) {
		PyErr_SetString(PyExc_TypeError, "Must pass a Name as arg 2");
		return NULL;
	}

	/* I think we should use this to do type checks -- Derek */
	if (!PyObject_IsInstance(py_closure, g_type_Closure)) {
		PyErr_SetString(PyExc_TypeError, "Must pass a Closure as arg 3");
		return NULL;
	}

	if (strcmp(py_templ->ob_type->tp_name, "Interest") != 0) {
		PyErr_SetString(PyExc_TypeError, "Must pass an Interest as arg 4");
		return NULL;
	}

	// Dereference the CCN handle, name, and template

	py_o = PyObject_GetAttrString(py_ccn, "ccn_data");
	if (!py_o)
		return NULL;
	handle = CCNObject_Get(HANDLE, py_o);
	Py_DECREF(py_o);

	py_o = PyObject_GetAttrString(py_name, "ccn_data");
	if (!py_o)
		return NULL;
	name = CCNObject_Get(NAME, py_o);
	Py_DECREF(py_o);

	py_o = PyObject_GetAttrString(py_templ, "ccn_data");
	if (!py_o)
		return NULL;
	Py_DECREF(py_o);
	templ = CCNObject_Get(INTEREST, py_o);

	// Build the closure
	py_o = CCNObject_New_Closure(&cl);
	cl->p = ccn_upcall_handler;
	cl->data = py_o;
	Py_INCREF(py_closure); /* We don't want py_closure to be dealocated */
	r = PyCapsule_SetContext(py_o, py_closure);
	assert(r == 0);

	/* I don't think Closure needs this, the information is only valid
	 * for time the interest is issued, it would also complicate things
	 * if the same closure would be used multiple times -- Derek
	 */
#if 0
	PyObject_SetAttrString(py_closure, "ccn_data", py_o);
	PyObject_GC_Track(py_o); //Add object to cyclic garbage collector
	PyObject_GC_Track(py_closure);
#endif

	r = ccn_express_interest(handle, name, cl, templ);
	if (r < 0) {
		int err = ccn_geterror(handle);

		Py_DECREF(py_o);
		PyErr_Format(PyExc_IOError, "Unable to issue an interest: %s [%d]",
				strerror(err), err);
		return NULL;
	}

	/*
	 * We aren't decreasing reference to py_o, because we're expecting
	 * to ccn call our hook where we will do it
	 */

	Py_RETURN_NONE;
}

static PyObject *
_pyccn_ccn_set_interest_filter(PyObject *UNUSED(self), PyObject *args)
{
	PyObject *py_ccn, *py_name, *py_closure, *py_o;
	int forw_flags = CCN_FORW_ACTIVE | CCN_FORW_CHILD_INHERIT;
	struct ccn *handle;
	struct ccn_charbuf *name;
	struct ccn_closure *closure;
	int r;

	if (!PyArg_ParseTuple(args, "OOO|i", &py_ccn, &py_name, &py_closure,
			&forw_flags))
		return NULL;

	if (!CCNObject_IsValid(HANDLE, py_ccn)) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN handle as arg 1");
		return NULL;
	}

	if (!CCNObject_IsValid(NAME, py_name)) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN Name as arg 1");
		return NULL;
	}

	if (!PyObject_IsInstance(py_closure, g_type_Closure)) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN Closure as arg 3");
		return NULL;
	}

	handle = CCNObject_Get(HANDLE, py_ccn);
	name = CCNObject_Get(NAME, py_name);

	/*
	 * This code it might be confusing so here is what it does:
	 * 1. we allocate a closure structure and wrap it into PyCapsule, so we can
	 *    easily do garbage collection. Decreasing reference count will
	 *    deallocate everything
	 * 2. set our closure handler
	 * 3. set pointer to our capsule (that way when callback is triggered we
	 *    have access to Python closure object)
	 * 4. increase reference count for Closure object to make sure someone
	 *    won't free it
	 * 5. we add pointer for our closure class (so we can call correct method)
	 */
	py_o = CCNObject_New_Closure(&closure);
	closure->p = ccn_upcall_handler;
	closure->data = py_o;
	Py_INCREF(py_closure);
	r = PyCapsule_SetContext(py_o, py_closure);
	assert(r == 0);

	r = ccn_set_interest_filter_with_flags(handle, name, closure, forw_flags);
	if (r < 0) {
		int err = ccn_geterror(handle);

		Py_DECREF(py_o);
		PyErr_Format(PyExc_IOError, "Unable to set and interest filter: %s [%d]",
				strerror(err), err);
		return NULL;
	}

	return Py_BuildValue("i", r);
}

// Simple get/put

static PyObject *
_pyccn_ccn_get(PyObject *UNUSED(self), PyObject *args)
{
	PyObject *py_CCN, *py_Name, *py_Interest = Py_None;
	PyObject *py_co = NULL, *py_o = NULL;
	PyObject *py_data = NULL, *py_pco = NULL, *py_comps = NULL;
	int r, timeout = 3000;
	struct ccn *handle;
	struct ccn_charbuf *name, *interest, *data;
	struct ccn_parsed_ContentObject *pco;
	struct ccn_indexbuf *comps;

	if (!PyArg_ParseTuple(args, "OO|Oi", &py_CCN, &py_Name, &py_Interest,
			&timeout))
		return NULL;

	if (strcmp(py_CCN->ob_type->tp_name, "CCN")) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN as arg 1");
		return NULL;
	} else {
		py_o = PyObject_GetAttrString(py_CCN, "ccn_data");
		JUMP_IF_NULL(py_o, exit);
		handle = CCNObject_Get(HANDLE, py_o);
		JUMP_IF_NULL(handle, exit);
		Py_CLEAR(py_o);
	}

	if (strcmp(py_Name->ob_type->tp_name, "Name")) {
		PyErr_SetString(PyExc_TypeError, "Must pass a Name as arg 2");
		return NULL;
	} else {
		py_o = PyObject_GetAttrString(py_Name, "ccn_data");
		JUMP_IF_NULL(py_o, exit);
		name = CCNObject_Get(NAME, py_o);
		JUMP_IF_NULL(name, exit);
		Py_CLEAR(py_o);
	}

	assert(py_Interest);
	if (py_Interest != Py_None && strcmp(py_Interest->ob_type->tp_name, "Interest")) {
		PyErr_SetString(PyExc_TypeError, "Must pass an Interest as arg 3");
		return NULL;
	} else if (py_Interest == Py_None)
		interest = NULL;
	else {
		py_o = PyObject_GetAttrString(py_Interest, "ccn_data");
		JUMP_IF_NULL(py_o, exit);
		interest = CCNObject_Get(INTEREST, py_o);
		JUMP_IF_NULL(interest, exit);
		Py_CLEAR(py_o);
	}

	py_data = CCNObject_New_charbuf(CONTENT_OBJECT, &data);
	JUMP_IF_NULL(py_data, exit);
	py_pco = CCNObject_New_ParsedContentObject(&pco);
	JUMP_IF_NULL(py_pco, exit);
	py_comps = CCNObject_New_ContentObjectComponents(&comps);
	JUMP_IF_NULL(py_comps, exit);

	Py_BEGIN_ALLOW_THREADS
	r = ccn_get(handle, name, interest, timeout, data, pco, comps, 0);
	Py_END_ALLOW_THREADS

	debug("ccn_get result=%d\n", r);

	if (r < 0) {
		//CCN doesn't clearly say when timeout happens, we're assuming
		//it is when no error was set
		int err = ccn_geterror(handle);
		if (err)
			py_co = PyErr_Format(PyExc_IOError, "%s [%d]", strerror(err), err);
		else
			py_co = (Py_INCREF(Py_None), Py_None); // timeout
	} else
		py_co = ContentObject_from_ccn_parsed(py_data, py_pco, py_comps);

exit:
	Py_XDECREF(py_comps);
	Py_XDECREF(py_pco);
	Py_XDECREF(py_data);
	Py_XDECREF(py_o);
	return py_co;
}

static PyObject * // int
_pyccn_ccn_put(PyObject *UNUSED(self), PyObject *args)
{
	PyObject *py_ccn, *py_content_object;
	PyObject *py_o;
	struct ccn_charbuf *content_object;
	struct ccn *handle;
	int r;

	if (!PyArg_ParseTuple(args, "OO", &py_ccn, &py_content_object))
		return NULL;

	if (strcmp(py_ccn->ob_type->tp_name, "CCN")) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN as arg 1");
		return NULL;
	}
	if (strcmp(py_content_object->ob_type->tp_name, "ContentObject")) {
		PyErr_SetString(PyExc_TypeError, "Must pass a ContentObject as arg 2");
		return NULL;
	}

	py_o = PyObject_GetAttrString(py_ccn, "ccn_data");
	assert(py_o);
	handle = CCNObject_Get(HANDLE, py_o);
	Py_DECREF(py_o);
	assert(handle);

	py_o = PyObject_GetAttrString(py_content_object, "ccn_data");
	assert(py_o);
	content_object = CCNObject_Get(CONTENT_OBJECT, py_o);
	Py_DECREF(py_o);
	assert(content_object);

	r = ccn_put(handle, content_object->buf, content_object->length);

	return Py_BuildValue("i", r);
}

// Keys

// TODO: Revise to make a method of CCN?
//
// args:  Key to fill, CCN Handle

static PyObject *
_pyccn_ccn_get_default_key(PyObject *UNUSED(self), PyObject *py_obj_CCN)
{
	struct ccn_keystore *keystore;
	const struct ccn_pkey *private_key;
	PyObject *py_handle;
	int r;

	debug("Got _pyccn_ccn_get_default_key start\n");

	if (strcmp(py_obj_CCN->ob_type->tp_name, "CCN")) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN Handle");
		return NULL;
	}

	struct ccn_private {
		int sock;
		size_t outbufindex;
		struct ccn_charbuf *interestbuf;
		struct ccn_charbuf *inbuf;
		struct ccn_charbuf *outbuf;
		struct ccn_charbuf *ccndid;
		struct hashtb *interests_by_prefix;
		struct hashtb *interest_filters;
		struct ccn_skeleton_decoder decoder;
		struct ccn_indexbuf *scratch_indexbuf;
		struct hashtb *keys; /* public keys, by pubid */
		struct hashtb *keystores; /* unlocked private keys */
		struct ccn_charbuf *default_pubid;
		struct timeval now;
		int timeout;
		int refresh_us;
		int err; /* pos => errno value, neg => other */
		int errline;
		int verbose_error;
		int tap;
		int running;
	};

	// In order to get the default key, have to call ccn_chk_signing_params
	// which seems to get the key and insert it in the hash table; otherwise
	// the hashtable starts empty
	// Could we just have an API call that returns the default signing key?
	//
	py_handle = PyObject_GetAttrString(py_obj_CCN, "ccn_data");
	JUMP_IF_NULL(py_handle, error);

	struct ccn_private *handle = CCNObject_Get(HANDLE, py_handle);
	struct ccn_signing_params name_sp = CCN_SIGNING_PARAMS_INIT;
	struct ccn_signing_params p = CCN_SIGNING_PARAMS_INIT;
	struct ccn_charbuf *timestamp = NULL;
	struct ccn_charbuf *finalblockid = NULL;
	struct ccn_charbuf *keylocator = NULL;
	r = ccn_chk_signing_params((struct ccn*) handle, &name_sp, &p,
			&timestamp, &finalblockid, &keylocator);
	if (r < 0) {
		PyErr_SetString(g_PyExc_CCNError, "Error while calling"
				" ccn_chk_signing_params()");
		goto error;
	}

	struct hashtb_enumerator ee;
	struct hashtb_enumerator *e = &ee;

	hashtb_start(handle->keystores, e);
	if (hashtb_seek(e, p.pubid, sizeof(p.pubid), 0) != HT_OLD_ENTRY) {
		debug("No default keystore?\n");
		hashtb_end(e);

		return(Py_INCREF(Py_None), Py_None);
	} else {
		struct ccn_keystore **pk = e->data;
		keystore = *pk;
		private_key = (struct ccn_pkey*) ccn_keystore_private_key(keystore);
	}
	hashtb_end(e);

	return Key_from_ccn((struct ccn_pkey*) private_key);

error:
	Py_XDECREF(py_handle);
	return NULL;
}

// We do not use these because working with the key storage
// in the library requires objects to have a handle to a CCN
// library, which is unnecessary.  Also, the hashtable storing
// keys in the library and keystore type itself is opaque to
// applications.
// So, Python users will have to come up with their own keystores.
/*

 static PyObject* // int
_pyccn_ccn_load_default_key(PyObject* self, PyObject* args) {
	return 0;
}
static PyObject*  // publisherID
 _pyccn_ccn_load_private_key(PyObject* self, PyObject* args) {
		// PyObject* key) {
	return 0; // publisher ID
}
static PyObject*  // pkey
_pyccn_ccn_get_public_key(PyObject* self, PyObject* args) {
	return 0;
}
 */

// TODO: Revise Python library to make a method of Key?
//

static PyObject*
_pyccn_generate_RSA_key(PyObject *UNUSED(self), PyObject *args)
{
	PyObject *py_key, *p;
	long keylen = 0;
	struct ccn_pkey *private_key, *public_key;
	unsigned char* public_key_digest;
	size_t public_key_digest_len;
	int result;

	if (!PyArg_ParseTuple(args, "Ol", &py_key, &keylen))
		return Py_BuildValue("i", -1); //TODO: Throw an error

	if (strcmp(py_key->ob_type->tp_name, "Key")) {
		PyErr_SetString(PyExc_TypeError, "Must pass a Key");
		return NULL;
	}

	generate_key(keylen, &private_key, &public_key, &public_key_digest, &public_key_digest_len);

	// privateKey
	// Don't free these here, python will call destructor
	p = CCNObject_New(PKEY, private_key);
	PyObject_SetAttrString(py_key, "ccn_data_private", p);
	Py_DECREF(p);

	// publicKey
	// Don't free this here, python will call destructor
	p = CCNObject_New(PKEY, public_key);
	PyObject_SetAttrString(py_key, "ccn_data_public", p);
	Py_DECREF(p);

	// type
	p = PyString_FromString("RSA");
	PyObject_SetAttrString(py_key, "type", p);
	Py_DECREF(p);

	// publicKeyID
	p = PyByteArray_FromStringAndSize((char*) public_key_digest, public_key_digest_len);
	PyObject_SetAttrString(py_key, "publicKeyID", p);
	Py_DECREF(p);
	free(public_key_digest);

	// publicKeyIDsize
	p = PyInt_FromLong(public_key_digest_len);
	PyObject_SetAttrString(py_key, "publicKeyIDsize", p);
	Py_DECREF(p);

	// pubID
	// TODO: pubID not implemented
	p = Py_None;
	PyObject_SetAttrString(py_key, "pubID", p);

	result = 0;

	return Py_BuildValue("i", result);
}


// ** Methods of SignedInfo
//
// Signing
/* We don't expose this because ccn_signing_params is not that useful to us
 * see comments above on this.
static PyObject* // int
_pyccn_ccn_chk_signing_params(PyObject* self, PyObject* args) {
	// Build internal signing params struct
	return 0;
}
 */

/* We don't expose this because it is done automatically in the Python SignedInfo object

static PyObject*
_pyccn_ccn_signed_info_create(PyObject* self, PyObject* args) {
	return 0;
}

 */

// ************
// SigningParams
//
//

// Note that SigningParams information is essentially redundant
// to what's in SignedInfo, and is internal to the
// ccn libraries.
// See the source for ccn_sign_content, for example.
//
// To use it requires working with keystores & hashtables to
// reference keys, which requires accessing private functions in the library
//
// So, we don't provide "to_ccn" functionality here, only "from_ccn" in case
// there is a need to parse a struct coming from the c library.


// Can be called directly from c library
//
// Pointer to a struct ccn_signing_params
//

static PyObject*
obj_SigningParams_from_ccn(PyObject *py_signing_params)
{
	struct ccn_signing_params *signing_params;
	PyObject *py_obj_SigningParams, *py_o;
	int r;

	assert(g_type_SigningParams);

	debug("SigningParams_from_ccn start\n");

	signing_params = CCNObject_Get(SIGNING_PARAMS, py_signing_params);

	// 1) Create python object
	py_obj_SigningParams = PyObject_CallObject(g_type_SigningParams, NULL);
	JUMP_IF_NULL(py_obj_SigningParams, error);

	// 2) Set ccn_data to a cobject pointing to the c struct
	//    and ensure proper destructor is set up for the c object.
	r = PyObject_SetAttrString(py_obj_SigningParams, "ccn_data",
			py_signing_params);
	JUMP_IF_NEG(r, error);


	// 3) Parse c structure and fill python attributes
	//    using PyObject_SetAttrString

	py_o = PyInt_FromLong(signing_params->sp_flags);
	JUMP_IF_NULL(py_o, error);
	r = PyObject_SetAttrString(py_obj_SigningParams, "flags", py_o);
	Py_DECREF(py_o);
	JUMP_IF_NEG(r, error);

	py_o = PyInt_FromLong(signing_params->type);
	JUMP_IF_NULL(py_o, error);
	r = PyObject_SetAttrString(py_obj_SigningParams, "type", py_o);
	Py_DECREF(py_o);
	JUMP_IF_NEG(r, error);

	py_o = PyInt_FromLong(signing_params->freshness);
	JUMP_IF_NULL(py_o, error);
	r = PyObject_SetAttrString(py_obj_SigningParams, "freshness", py_o);
	Py_DECREF(py_o);
	JUMP_IF_NEG(r, error);

	py_o = PyInt_FromLong(signing_params->api_version);
	JUMP_IF_NULL(py_o, error);
	r = PyObject_SetAttrString(py_obj_SigningParams, "apiVersion", py_o);
	Py_DECREF(py_o);
	JUMP_IF_NEG(r, error);

	if (signing_params->template_ccnb &&
			signing_params->template_ccnb->length > 0) {
		PyObject *py_signed_object;
		struct ccn_charbuf *signed_object;

		py_signed_object = CCNObject_New_charbuf(SIGNED_INFO, &signed_object);
		JUMP_IF_NULL(py_signed_object, error);

		r = ccn_charbuf_append_charbuf(signed_object,
				signing_params->template_ccnb);
		if (r < 0) {
			Py_DECREF(py_signed_object);
			PyErr_NoMemory();
			goto error;
		}

		py_o = SignedInfo_obj_from_ccn(py_signed_object);
		Py_DECREF(py_signed_object);
		JUMP_IF_NULL(py_o, error);
	} else
		py_o = (Py_INCREF(Py_None), Py_None);

	r = PyObject_SetAttrString(py_obj_SigningParams, "template", py_o);
	Py_DECREF(py_o);

	// Right now we're going to set this to the byte array corresponding
	// to the key hash, but this is not ideal
	// TODO:  Figure out how to deal with keys here...
	py_o = PyByteArray_FromStringAndSize((char *) signing_params->pubid,
			sizeof(signing_params->pubid));
	JUMP_IF_NULL(py_o, error);
	r = PyObject_SetAttrString(py_obj_SigningParams, "key", py_o);
	Py_DECREF(py_o);
	JUMP_IF_NEG(r, error);

	// 4) Return the created object
	debug("SigningParams_from_ccn ends\n");

	return py_obj_SigningParams;

error:
	Py_XDECREF(py_obj_SigningParams);
	return NULL;
}

static PyObject*
_pyccn_SigningParams_from_ccn(PyObject *UNUSED(self),
		PyObject *py_signing_params)
{
	if (!CCNObject_IsValid(SIGNING_PARAMS, py_signing_params)) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN SigningParams");
		return NULL;
	}

	return obj_SigningParams_from_ccn(py_signing_params);
}

/*
 * XXX: This looks like an useless function, the upcall_info supposed to be
 * just temporary structure valid for the duration of upcall, we shouldn't
 * ever have need to store it. If it indeed is needed, we need to make copy
 * of the struct and also write routine in destructor
 */
static PyObject *
_pyccn_UpcallInfo_from_ccn(PyObject *UNUSED(self), PyObject *py_upcall_info)
{
	struct ccn_upcall_info *upcall_info;

	if (!CCNObject_IsValid(UPCALL_INFO, py_upcall_info)) {
		PyErr_SetString(PyExc_TypeError, "Must pass a CCN UpcallInfo");
		return NULL;
	}

	upcall_info = CCNObject_Get(UPCALL_INFO, py_upcall_info);

	assert(0);
	//TODO: we need kind of interest as well!
	return obj_UpcallInfo_from_ccn(CCN_UPCALL_FINAL, upcall_info);
}

// DECLARATION OF PYTHON-ACCESSIBLE FUNCTIONS
//

static PyMethodDef _module_methods[] = {
	// ** Methods of CCN
	//
	{"_pyccn_ccn_create", _pyccn_ccn_create, METH_NOARGS, NULL},
	{"_pyccn_ccn_connect", _pyccn_ccn_connect, METH_O, NULL},
	{"_pyccn_ccn_disconnect", _pyccn_ccn_disconnect, METH_O, NULL},
	{"_pyccn_ccn_run", _pyccn_ccn_run, METH_VARARGS, NULL},
	{"_pyccn_ccn_set_run_timeout", _pyccn_ccn_set_run_timeout, METH_VARARGS,
		NULL},
	{"_pyccn_ccn_express_interest", _pyccn_ccn_express_interest, METH_VARARGS,
		NULL},
	{"_pyccn_ccn_set_interest_filter", _pyccn_ccn_set_interest_filter,
		METH_VARARGS, NULL},
	{"_pyccn_ccn_get", _pyccn_ccn_get, METH_VARARGS, NULL},
	{"_pyccn_ccn_put", _pyccn_ccn_put, METH_VARARGS, NULL},
	{"_pyccn_ccn_get_default_key", _pyccn_ccn_get_default_key, METH_O, NULL},
#if 0
	{"_pyccn_ccn_load_default_key", _pyccn_ccn_load_default_key, METH_VARARGS,
		""},
	{"_pyccn_ccn_load_private_key", _pyccn_ccn_load_private_key, METH_VARARGS,
		""},
	{"_pyccn_ccn_get_public_key", _pyccn_ccn_get_public_key, METH_VARARGS,
		""},
#endif
	{"_pyccn_generate_RSA_key", _pyccn_generate_RSA_key, METH_VARARGS,
		""},

	// ** Methods of ContentObject
	//
	{"content_to_bytearray", _pyccn_content_to_bytearray, METH_O, NULL},
#if 0
	{"_pyccn_ccn_encode_content_object", _pyccn_ccn_encode_content_object, METH_VARARGS,
		""},
	{"_pyccn_ccn_verify_content", _pyccn_ccn_verify_content, METH_VARARGS,
		""},
	{"_pyccn_ccn_content_matches_interest", _pyccn_ccn_content_matches_interest, METH_VARARGS,
		""},
#endif
#if 0
	{"_pyccn_ccn_chk_signing_params", _pyccn_ccn_chk_signing_params, METH_VARARGS,
		""},
	{"_pyccn_ccn_signed_info_create", _pyccn_ccn_signed_info_create, METH_VARARGS,
		""},
#endif
	// Naming
#if 0
	{"_pyccn_ccn_name_init", _pyccn_ccn_name_init, METH_VARARGS,
		""},
	{"_pyccn_ccn_name_append_nonce", _pyccn_ccn_name_append_nonce, METH_VARARGS,
		""},
	{"_pyccn_ccn_compare_names", _pyccn_ccn_compare_names, METH_VARARGS,
		""},
#endif
	// Converters
	{"_pyccn_Name_to_ccn", _pyccn_Name_to_ccn, METH_O, NULL},
	{"_pyccn_Name_from_ccn", _pyccn_Name_from_ccn, METH_O, NULL},
	{"_pyccn_Interest_to_ccn", _pyccn_Interest_to_ccn, METH_O, NULL},
	{"_pyccn_Interest_from_ccn", _pyccn_Interest_from_ccn, METH_VARARGS,
		""},
	{"_pyccn_ContentObject_to_ccn", _pyccn_ContentObject_to_ccn, METH_VARARGS,
		""},
#if 0
	{"_pyccn_ContentObject_from_ccn", _pyccn_ContentObject_from_ccn, METH_VARARGS,
		""},
#endif
	{"_pyccn_Key_to_ccn_public", _pyccn_Key_to_ccn_public, METH_O, NULL},
	{"_pyccn_Key_to_ccn_private", _pyccn_Key_to_ccn_private, METH_O, NULL},
	{"_pyccn_Key_from_ccn", _pyccn_Key_from_ccn, METH_O, NULL},
	{"_pyccn_KeyLocator_to_ccn", (PyCFunction) _pyccn_KeyLocator_to_ccn,
		METH_VARARGS | METH_KEYWORDS, NULL},
	{"_pyccn_KeyLocator_from_ccn", _pyccn_KeyLocator_from_ccn, METH_O, NULL},
	{"_pyccn_Signature_to_ccn", _pyccn_Signature_to_ccn, METH_O, NULL},
	{"_pyccn_Signature_from_ccn", _pyccn_Signature_from_ccn, METH_O, NULL},
	{"_pyccn_SignedInfo_to_ccn", (PyCFunction) _pyccn_SignedInfo_to_ccn,
		METH_VARARGS | METH_KEYWORDS, NULL},
	{"_pyccn_SignedInfo_from_ccn", _pyccn_SignedInfo_from_ccn, METH_O, NULL},
#if 0
	{"_pyccn_SignedInfo_to_ccn", _pyccn_SigningParams_to_ccn, METH_VARARGS,
		""},
#endif
	{"_pyccn_SignedInfo_from_ccn", _pyccn_SigningParams_from_ccn, METH_O, NULL},
	{"_pyccn_ExclusionFilter_to_ccn", _pyccn_ExclusionFilter_to_ccn, METH_O,
		NULL},
	{"_pyccn_ExclusionFilter_from_ccn", _pyccn_ExclusionFilter_from_ccn, METH_O,
		NULL},
	{"_pyccn_UpcallInfo_from_ccn", _pyccn_UpcallInfo_from_ccn, METH_O, NULL},

	{NULL, NULL, 0, NULL} /* Sentinel */
};

PyObject *
initialize_methods(const char* name)
{
	return Py_InitModule(name, _module_methods);
}