/* ----------------------------------------------------------------------------
 * This file was automatically generated by SWIG (http://www.swig.org).
 * Version 2.0.7
 *
 * Do not make changes to this file unless you know what you are doing--modify
 * the SWIG interface file instead.
 * ----------------------------------------------------------------------------- */

namespace Scalien {

using System;
using System.Runtime.InteropServices;

public class SDBP_Buffer : IDisposable {
  private HandleRef swigCPtr;
  protected bool swigCMemOwn;

  internal SDBP_Buffer(IntPtr cPtr, bool cMemoryOwn) {
    swigCMemOwn = cMemoryOwn;
    swigCPtr = new HandleRef(this, cPtr);
  }

  internal static HandleRef getCPtr(SDBP_Buffer obj) {
    return (obj == null) ? new HandleRef(null, IntPtr.Zero) : obj.swigCPtr;
  }

  ~SDBP_Buffer() {
    Dispose();
  }

  public virtual void Dispose() {
    lock(this) {
      if (swigCPtr.Handle != IntPtr.Zero) {
        if (swigCMemOwn) {
          swigCMemOwn = false;
          scaliendb_clientPINVOKE.delete_SDBP_Buffer(swigCPtr);
        }
        swigCPtr = new HandleRef(null, IntPtr.Zero);
      }
      GC.SuppressFinalize(this);
    }
  }

  public unsafe SDBP_Buffer() : this(scaliendb_clientPINVOKE.new_SDBP_Buffer(), true) {
  }

  public unsafe void SetBuffer(IntPtr data_, int len_) {
    scaliendb_clientPINVOKE.SDBP_Buffer_SetBuffer(swigCPtr, data_, len_);
  }

  public unsafe IntPtr data {
    set {
      scaliendb_clientPINVOKE.SDBP_Buffer_data_set(swigCPtr, value);
    } 
    /* HACKED SWIG csvarout typemap in scaliendb_client.i  */
    get {
		return scaliendb_clientPINVOKE.SDBP_Buffer_data_get(swigCPtr);
    } 
  }

  public unsafe int len {
    set {
      scaliendb_clientPINVOKE.SDBP_Buffer_len_set(swigCPtr, value);
    } 
    get {
      int ret = scaliendb_clientPINVOKE.SDBP_Buffer_len_get(swigCPtr);
      return ret;
    } 
  }

}

}
