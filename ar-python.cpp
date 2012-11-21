// Copyright (C) 2012 Rhys Ulerich
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// TODO Figure out building extensions in C++ mode

#include <cstdlib>
#include <string>
#include <vector>

#include <Python.h>
#include <numpy/arrayobject.h>

#include "ar.hpp"

// Compile-time defaults in the code also appearing in arsel docstring
#define DEFAULT_SUBMEAN   true
#define DEFAULT_ABSRHO    false
#define DEFAULT_CRITERION "CIC"
#define DEFAULT_MAXORDER  512
#define STRINGIFY(x) STRINGIFY_HELPER(x)
#define STRINGIFY_HELPER(x) #x

static const char ar_arsel_docstring[] =
"    Usage: M = arsel (data, submean, absrho, criterion, maxorder)\n"
"\n"
"    Automatically fit autoregressive models to input signals.\n"
"\n"
"    Use ar::burg_method and ar::best_model to fit an autoregressive process\n"
"    for signals contained in the rows of matrix data.  Sample means will\n"
"    be subtracted whenever submean is true.  Model orders zero through\n"
"    min(columns(data), maxorder) will be considered.  A dictionary is\n"
"    returned where each key either contains a result indexable by the\n"
"    signal number (i.e. the row indices of input matrix data) or it contains\n"
"    a single scalar applicable to all signals.\n"
"\n"
"    The model order will be selected using the specified criterion.\n"
"    Criteria are specified using the following abbreviations:\n"
"        AIC  - Akaike information criterion\n"
"        AICC - asymptotically-corrected Akaike information criterion\n"
"        BIC  - consistent criterion BIC\n"
"        CIC  - combined information criterion\n"
"        FIC  - finite information criterion\n"
"        FSIC - finite sample information criterion\n"
"        GIC  - generalized information criterion\n"
"        MCC  - minimally consistent criterion\n"
"\n"
"    The number of samples in data (i.e. the number of rows) is returned\n"
"    in key 'N'.  The filter()-ready process parameters are returned\n"
"    in key 'AR', the sample mean in 'mu', and the innovation variance\n"
"    \\sigma^2_\\epsilon in 'sigma2eps'.  The process output variance\n"
"    \\sigma^2_\\x and process gain are returned in keys 'sigma2x' and\n"
"    'gain', respectively.  Autocorrelations for lags zero through the\n"
"    model order, inclusive, are returned in key 'autocor'.  The raw\n"
"    signals are made available for later use in field 'data'.\n"
"\n"
"    Given the observed autocorrelation structure, a decorrelation time\n"
"    'T0' is computed by ar::decorrelation_time and used to estimate\n"
"    the effective signal variance 'eff_var'.  The number of effectively\n"
"    independent samples is returned in 'eff_N'.  These effective values are\n"
"    combined to estimate the sampling error (i.e. the standard deviation\n"
"    of the sample mean) as field 'mu_sigma'.  The absolute value of the\n"
"    autocorrelation function will be used in computing the decorrelation\n"
"    times whenever absrho is true.\n"
"\n"
"    When omitted, submean defaults to " STRINGIFY(DEFAULT_SUBMEAN) ".\n"
"    When omitted, absrho defaults to " STRINGIFY(DEFAULT_ABSRHO) ".\n"
"    When omitted, criterion defaults to " STRINGIFY(DEFAULT_CRITERION) ".\n"
"    When omitted, maxorder defaults to " STRINGIFY(DEFAULT_MAXORDER) ".\n"
;

extern "C" PyObject *ar_arsel(PyObject *self, PyObject *args)
{
    // Prepare argument storage with default values
    PyObject   *data_obj  = NULL;
    int         submean   = DEFAULT_SUBMEAN;
    int         absrho    = DEFAULT_ABSRHO;
    const char *criterion = DEFAULT_CRITERION;
    std::size_t maxorder  = DEFAULT_MAXORDER;

    // Parse input tuple with second and subsequent arguments optional
    {
        unsigned long ul_maxorder = maxorder;
        if (!PyArg_ParseTuple(args, "O|iisk", &data_obj, &submean, &absrho,
                                            &criterion, &ul_maxorder)) {
            return NULL;
        }
        maxorder = ul_maxorder;
    }

    // Lookup the desired model selection criterion
    typedef ar::best_model_function<
            ar::Burg,npy_intp,std::vector<double> > best_model_function;
    const best_model_function::type best_model
            = best_model_function::lookup(std::string(criterion), submean);
    if (!best_model) {
        PyErr_SetString(PyExc_RuntimeError,
            "Unknown model selection criterion provided to arsel.");
        return NULL;
    }

    // Incoming data may be noncontiguous but should otherwise be well-behaved
    // On success, 'data' is returned so Py_DECREF is applied only on failure
    PyObject *data = PyArray_FROMANY(data_obj, NPY_DOUBLE, 2, 2,
            NPY_ALIGNED | NPY_ELEMENTSTRIDES | NPY_NOTSWAPPED);
    if (!data) {
        Py_DECREF(data);
        return NULL;
    }

    // How many data points are there?
    npy_intp M = PyArray_DIM(data, 0);
    npy_intp N = PyArray_DIM(data, 1);

    // Prepare per-signal storage locations to return to caller
    // TODO Ensure these invocations all worked as expected
    PyObject *_AR        = PyList_New(M);
    for (npy_intp k = 0; k < M; ++k) {
        PyList_SetItem(_AR, k, PyList_New(0));
    }
    PyObject *_autocor   = PyList_New(M);
    for (npy_intp k = 0; k < M; ++k) {
        PyList_SetItem(_autocor, k, PyList_New(0));
    }
    PyObject *_eff_N     = PyArray_ZEROS(1, &M, NPY_DOUBLE, 0);
    PyObject *_eff_var   = PyArray_ZEROS(1, &M, NPY_DOUBLE, 0);
    PyObject *_gain      = PyArray_ZEROS(1, &M, NPY_DOUBLE, 0);
    PyObject *_mu        = PyArray_ZEROS(1, &M, NPY_DOUBLE, 0);
    PyObject *_mu_sigma  = PyArray_ZEROS(1, &M, NPY_DOUBLE, 0);
    PyObject *_sigma2eps = PyArray_ZEROS(1, &M, NPY_DOUBLE, 0);
    PyObject *_sigma2x   = PyArray_ZEROS(1, &M, NPY_DOUBLE, 0);
    PyObject *_T0        = PyArray_ZEROS(1, &M, NPY_DOUBLE, 0);

    // Prepare vectors to capture burg_method() output
    std::vector<double> params, sigma2e, gain, autocor;
    params .reserve(maxorder*(maxorder + 1)/2);
    sigma2e.reserve(maxorder + 1);
    gain   .reserve(maxorder + 1);
    autocor.reserve(maxorder + 1);

    // Prepare repeatedly-used working storage for burg_method()
    std::vector<double> f, b, Ak, ac;

    // Process each signal in turn...
    for (npy_intp i = 0; i < M; ++i)
    {
        // Use burg_method to estimate a hierarchy of AR models from input data
        params .clear();
        sigma2e.clear();
        gain   .clear();
        autocor.clear();
        ar::strided_adaptor<const double*> signal_begin(
                (const double*) PyArray_GETPTR2(data, i, 0),
                PyArray_STRIDES(data)[1] / sizeof(double));
        ar::strided_adaptor<const double*> signal_end  (
                (const double*) PyArray_GETPTR2(data, i, N),
                PyArray_STRIDES(data)[1] / sizeof(double));
        ar::burg_method(signal_begin, signal_end,
                        *(double*)PyArray_GETPTR1(_mu, i),
                        maxorder,
                        std::back_inserter(params),
                        std::back_inserter(sigma2e),
                        std::back_inserter(gain),
                        std::back_inserter(autocor),
                        submean, /* output hierarchy? */ true, f, b, Ak, ac);

        // Keep only best model per chosen criterion via function pointer
        best_model(N, params, sigma2e, gain, autocor);

        // Compute decorrelation time from the estimated autocorrelation model
        ar::predictor<double> p = ar::autocorrelation(
                params.begin(), params.end(), gain[0], autocor.begin());
        *(double*)PyArray_GETPTR1(_T0, i)
            = ar::decorrelation_time(N, p, absrho);

        // Filter()-ready process parameters in field 'AR' with leading one
        PyList_Append(_AR, PyFloat_FromDouble(1));
        for (std::vector<double>::iterator k = params.begin();
                k != params.end(); ++k) {
            PyList_Append(_AR, PyFloat_FromDouble(*k));
        }

        // Field 'sigma2eps'
        *(double*)PyArray_GETPTR1(_sigma2eps, i) = sigma2e[0];

        // Field 'gain'
        *(double*)PyArray_GETPTR1(_gain, i) = gain[0];

        // Field 'sigma2x'
        *(double*)PyArray_GETPTR1(_sigma2x, i) = gain[0]*sigma2e[0];

        // Field 'autocor'
        for (std::vector<double>::iterator k = autocor.begin();
                k != autocor.end(); ++k) {
            PyList_Append(_autocor, PyFloat_FromDouble(*k));
        }

        // Field 'eff_var'
        // Unbiased effective variance expression from [Trenberth1984]
        *(double*)PyArray_GETPTR1(_eff_var, i)
            = (N*gain[0]*sigma2e[0]) / (N - *(double*)PyArray_GETPTR1(_T0, i));

        // Field 'eff_N'
        *(double*)PyArray_GETPTR1(_eff_N, i)
            = N / *(double*)PyArray_GETPTR1(_T0, i);

        // Field 'mu_sigma'
        // Variance of the sample mean using effective quantities
        *(double*)PyArray_GETPTR1(_mu_sigma, i)
            = std::sqrt(   *(double*)PyArray_GETPTR1(_eff_var, i)
                         / *(double*)PyArray_GETPTR1(_eff_N,   i));

        // TODO Permit user to interrupt the computations at this time
    }

    // Allocate the dictionary returned by the method
    PyObject *ret = PyDict_New();
    if (!ret) goto fail;

    // Arguments preserved as outputs
    // TODO Use Py_BuildValue here?
    // TODO Ensure these invocations all worked as expected
    PyDict_SetItemString(ret, "data"     , data);
    PyDict_SetItemString(ret, "submean"  , PyBool_FromLong(submean));
    PyDict_SetItemString(ret, "absrho"   , PyBool_FromLong(absrho));
    PyDict_SetItemString(ret, "criterion", PyString_FromString(criterion));
    PyDict_SetItemString(ret, "maxorder" , PyLong_FromLong(maxorder));
    PyDict_SetItemString(ret, "N"        , PyLong_FromLong(N));

    // Computed results
    // TODO Ensure these invocations all worked as expected
    PyDict_SetItemString(ret, "AR"       , _AR       );
    PyDict_SetItemString(ret, "autocor"  , _autocor  );
    PyDict_SetItemString(ret, "eff_N"    , _eff_N    );
    PyDict_SetItemString(ret, "eff_var"  , _eff_var  );
    PyDict_SetItemString(ret, "gain"     , _gain     );
    PyDict_SetItemString(ret, "mu"       , _mu       );
    PyDict_SetItemString(ret, "mu_sigma" , _mu_sigma );
    PyDict_SetItemString(ret, "sigma2eps", _sigma2eps);
    PyDict_SetItemString(ret, "sigma2x"  , _sigma2x  );
    PyDict_SetItemString(ret, "T0"       , _T0       );

    return ret;

fail:
    Py_XDECREF(data);
    if (_AR) {
        const Py_ssize_t nelem = PyList_Size(_AR);
        for (Py_ssize_t i = 0; i < nelem; ++i) {
            Py_XDECREF(PyList_GetItem(_AR, i));
        }
        Py_DECREF(_AR);
    }
    if (_autocor) {
        const Py_ssize_t nelem = PyList_Size(_autocor);
        for (Py_ssize_t i = 0; i < nelem; ++i) {
            Py_XDECREF(PyList_GetItem(_autocor, i));
        }
        Py_DECREF(_autocor);
    }
    Py_XDECREF(_eff_N);
    Py_XDECREF(_eff_var);
    Py_XDECREF(_gain);
    Py_XDECREF(_mu);
    Py_XDECREF(_mu_sigma);
    Py_XDECREF(_sigma2eps);
    Py_XDECREF(_sigma2x);
    Py_XDECREF(_T0);
    return NULL;
}

// Specification of methods available in the module
static PyMethodDef ar_methods[] = {
    {"arsel", ar_arsel, METH_VARARGS, ar_arsel_docstring},
    {NULL, NULL, 0, NULL}
};

// Module docstring
static const char ar_docstring[] = "Autoregressive process modeling tools";

// Initialize the module, including making NumPy available
extern "C" PyMODINIT_FUNC initar(void)
{
    if (!Py_InitModule3("ar", ar_methods, ar_docstring)) return;
    import_array();
}