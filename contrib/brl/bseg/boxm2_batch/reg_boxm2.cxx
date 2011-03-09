#include "reg_boxm2.h"
#include <bprb/bprb_macros.h>
#include <bprb/bprb_batch_process_manager.h>
#include <brdb/brdb_selection.h>

// processes
#include <boxm2/pro/boxm2_register.h>

#if defined(HAS_OPENCL) && HAS_OPENCL
#include <bocl/pro/bocl_register.h>
#endif


PyObject *
register_processes(PyObject *self, PyObject *args)
{
  boxm2_register::register_process();
#if defined(HAS_OPENCL) && HAS_OPENCL
  bocl_register::register_process();
#endif
  Py_INCREF(Py_None);
  return Py_None;
}

PyObject *
register_datatypes(PyObject *self, PyObject *args)
{
  boxm2_register::register_datatype();
#if defined(HAS_OPENCL) && HAS_OPENCL
  bocl_register::register_datatype();
#endif
  Py_INCREF(Py_None);
  return Py_None;
}


PyMODINIT_FUNC
initboxm2_batch(void)
{
  PyMethodDef reg_pro;
  reg_pro.ml_name = "register_processes";
  reg_pro.ml_meth = register_processes;
  reg_pro.ml_doc = "register_processes() create instances of each defined process";
  reg_pro.ml_flags = METH_VARARGS;


  PyMethodDef reg_data;
  reg_data.ml_name = "register_datatypes";
  reg_data.ml_meth = register_datatypes;
  reg_data.ml_doc = "register_datatypes() insert tables in the database for each type";
  reg_data.ml_flags = METH_VARARGS;


  boxm2_batch_methods[0]=reg_pro;
  boxm2_batch_methods[1]=reg_data;

  for (int i=0; i<METHOD_NUM; ++i) {
    boxm2_batch_methods[i+2]=batch_methods[i];
  }

  Py_InitModule("boxm2_batch", boxm2_batch_methods);
}

