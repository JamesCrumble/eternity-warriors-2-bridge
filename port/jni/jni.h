/*
 * Copyright (C) 2006 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * JNI specification, as defined by Sun:
 * http://java.sun.com/javase/6/docs/technotes/guides/jni/spec/jniTOC.html
 *
 * Everything here is expected to be VM-neutral.
 */

#ifndef JNI_H_
#define JNI_H_
#if defined(__arm__)
#define JNI_ABI __attribute__((pcs("aapcs")))
#else
#define JNI_ABI
#endif

#include <stdarg.h>
#include <stdint.h>

/* Primitive types that match up with Java equivalents. */
typedef uint8_t  jboolean; /* unsigned 8 bits */
typedef int8_t   jbyte;    /* signed 8 bits */
typedef uint16_t jchar;    /* unsigned 16 bits */
typedef int16_t  jshort;   /* signed 16 bits */
typedef int32_t  jint;     /* signed 32 bits */
typedef int64_t  jlong;    /* signed 64 bits */
typedef float    jfloat;   /* 32-bit IEEE 754 */
typedef double   jdouble;  /* 64-bit IEEE 754 */

/* "cardinal indices and sizes" */
typedef jint     jsize;

#ifdef __cplusplus
/*
 * Reference types, in C++
 */
class _jobject {};
class _jclass : public _jobject {};
class _jstring : public _jobject {};
class _jarray : public _jobject {};
class _jobjectArray : public _jarray {};
class _jbooleanArray : public _jarray {};
class _jbyteArray : public _jarray {};
class _jcharArray : public _jarray {};
class _jshortArray : public _jarray {};
class _jintArray : public _jarray {};
class _jlongArray : public _jarray {};
class _jfloatArray : public _jarray {};
class _jdoubleArray : public _jarray {};
class _jthrowable : public _jobject {};

typedef _jobject*       jobject;
typedef _jclass*        jclass;
typedef _jstring*       jstring;
typedef _jarray*        jarray;
typedef _jobjectArray*  jobjectArray;
typedef _jbooleanArray* jbooleanArray;
typedef _jbyteArray*    jbyteArray;
typedef _jcharArray*    jcharArray;
typedef _jshortArray*   jshortArray;
typedef _jintArray*     jintArray;
typedef _jlongArray*    jlongArray;
typedef _jfloatArray*   jfloatArray;
typedef _jdoubleArray*  jdoubleArray;
typedef _jthrowable*    jthrowable;
typedef _jobject*       jweak;


#else /* not __cplusplus */

/*
 * Reference types, in C.
 */
typedef void*           jobject;
typedef jobject         jclass;
typedef jobject         jstring;
typedef jobject         jarray;
typedef jarray          jobjectArray;
typedef jarray          jbooleanArray;
typedef jarray          jbyteArray;
typedef jarray          jcharArray;
typedef jarray          jshortArray;
typedef jarray          jintArray;
typedef jarray          jlongArray;
typedef jarray          jfloatArray;
typedef jarray          jdoubleArray;
typedef jobject         jthrowable;
typedef jobject         jweak;

#endif /* not __cplusplus */

struct _jfieldID;                       /* opaque structure */
typedef struct _jfieldID* jfieldID;     /* field IDs */

struct _jmethodID;                      /* opaque structure */
typedef struct _jmethodID* jmethodID;   /* method IDs */

struct JNIInvokeInterface;

typedef union jvalue {
    jboolean    z;
    jbyte       b;
    jchar       c;
    jshort      s;
    jint        i;
    jlong       j;
    jfloat      f;
    jdouble     d;
    jobject     l;
} jvalue;

typedef enum jobjectRefType {
    JNIInvalidRefType = 0,
    JNILocalRefType = 1,
    JNIGlobalRefType = 2,
    JNIWeakGlobalRefType = 3
} jobjectRefType;

typedef struct {
    const char* name;
    const char* signature;
    void*       fnPtr;
} JNINativeMethod;

struct _JNIEnv;
struct _JavaVM;
typedef const struct JNINativeInterface* C_JNIEnv;

#if defined(__cplusplus)
typedef _JNIEnv JNIEnv;
typedef _JavaVM JavaVM;
#else
typedef const struct JNINativeInterface* JNIEnv;
typedef const struct JNIInvokeInterface* JavaVM;
#endif

/*
 * Table of interface function pointers.
 */
struct JNINativeInterface {
    void*       reserved0;
    void*       reserved1;
    void*       reserved2;
    void*       reserved3;

    jint        (*GetVersion)(JNIEnv *) JNI_ABI;

    jclass      (*DefineClass)(JNIEnv*, const char*, jobject, const jbyte*,
                        jsize) JNI_ABI;
    jclass      (*FindClass)(JNIEnv*, const char*) JNI_ABI;

    jmethodID   (*FromReflectedMethod)(JNIEnv*, jobject) JNI_ABI;
    jfieldID    (*FromReflectedField)(JNIEnv*, jobject) JNI_ABI;
    /* spec doesn't show jboolean parameter */
    jobject     (*ToReflectedMethod)(JNIEnv*, jclass, jmethodID, jboolean) JNI_ABI;

    jclass      (*GetSuperclass)(JNIEnv*, jclass) JNI_ABI;
    jboolean    (*IsAssignableFrom)(JNIEnv*, jclass, jclass) JNI_ABI;

    /* spec doesn't show jboolean parameter */
    jobject     (*ToReflectedField)(JNIEnv*, jclass, jfieldID, jboolean) JNI_ABI;

    jint        (*Throw)(JNIEnv*, jthrowable) JNI_ABI;
    jint        (*ThrowNew)(JNIEnv *, jclass, const char *) JNI_ABI;
    jthrowable  (*ExceptionOccurred)(JNIEnv*) JNI_ABI;
    void        (*ExceptionDescribe)(JNIEnv*) JNI_ABI;
    void        (*ExceptionClear)(JNIEnv*) JNI_ABI;
    void        (*FatalError)(JNIEnv*, const char*) JNI_ABI;

    jint        (*PushLocalFrame)(JNIEnv*, jint) JNI_ABI;
    jobject     (*PopLocalFrame)(JNIEnv*, jobject) JNI_ABI;

    jobject     (*NewGlobalRef)(JNIEnv*, jobject) JNI_ABI;
    void        (*DeleteGlobalRef)(JNIEnv*, jobject) JNI_ABI;
    void        (*DeleteLocalRef)(JNIEnv*, jobject) JNI_ABI;
    jboolean    (*IsSameObject)(JNIEnv*, jobject, jobject) JNI_ABI;

    jobject     (*NewLocalRef)(JNIEnv*, jobject) JNI_ABI;
    jint        (*EnsureLocalCapacity)(JNIEnv*, jint) JNI_ABI;

    jobject     (*AllocObject)(JNIEnv*, jclass) JNI_ABI;
    jobject     (*NewObject)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    jobject     (*NewObjectV)(JNIEnv*, jclass, jmethodID, va_list) JNI_ABI;
    jobject     (*NewObjectA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;

    jclass      (*GetObjectClass)(JNIEnv*, jobject) JNI_ABI;
    jboolean    (*IsInstanceOf)(JNIEnv*, jobject, jclass) JNI_ABI;
    jmethodID   (*GetMethodID)(JNIEnv*, jclass, const char*, const char*) JNI_ABI;

    jobject     (*CallObjectMethod)(JNIEnv*, jobject, jmethodID, ...) JNI_ABI;
    jobject     (*CallObjectMethodV)(JNIEnv*, jobject, jmethodID, va_list) JNI_ABI;
    jobject     (*CallObjectMethodA)(JNIEnv*, jobject, jmethodID, const jvalue*) JNI_ABI;
    jboolean    (*CallBooleanMethod)(JNIEnv*, jobject, jmethodID, ...) JNI_ABI;
    jboolean    (*CallBooleanMethodV)(JNIEnv*, jobject, jmethodID, va_list) JNI_ABI;
    jboolean    (*CallBooleanMethodA)(JNIEnv*, jobject, jmethodID, const jvalue*) JNI_ABI;
    jbyte       (*CallByteMethod)(JNIEnv*, jobject, jmethodID, ...) JNI_ABI;
    jbyte       (*CallByteMethodV)(JNIEnv*, jobject, jmethodID, va_list) JNI_ABI;
    jbyte       (*CallByteMethodA)(JNIEnv*, jobject, jmethodID, const jvalue*) JNI_ABI;
    jchar       (*CallCharMethod)(JNIEnv*, jobject, jmethodID, ...) JNI_ABI;
    jchar       (*CallCharMethodV)(JNIEnv*, jobject, jmethodID, va_list) JNI_ABI;
    jchar       (*CallCharMethodA)(JNIEnv*, jobject, jmethodID, const jvalue*) JNI_ABI;
    jshort      (*CallShortMethod)(JNIEnv*, jobject, jmethodID, ...) JNI_ABI;
    jshort      (*CallShortMethodV)(JNIEnv*, jobject, jmethodID, va_list) JNI_ABI;
    jshort      (*CallShortMethodA)(JNIEnv*, jobject, jmethodID, const jvalue*) JNI_ABI;
    jint        (*CallIntMethod)(JNIEnv*, jobject, jmethodID, ...) JNI_ABI;
    jint        (*CallIntMethodV)(JNIEnv*, jobject, jmethodID, va_list) JNI_ABI;
    jint        (*CallIntMethodA)(JNIEnv*, jobject, jmethodID, const jvalue*) JNI_ABI;
    jlong       (*CallLongMethod)(JNIEnv*, jobject, jmethodID, ...) JNI_ABI;
    jlong       (*CallLongMethodV)(JNIEnv*, jobject, jmethodID, va_list) JNI_ABI;
    jlong       (*CallLongMethodA)(JNIEnv*, jobject, jmethodID, const jvalue*) JNI_ABI;
    jfloat      (*CallFloatMethod)(JNIEnv*, jobject, jmethodID, ...) JNI_ABI;
    jfloat      (*CallFloatMethodV)(JNIEnv*, jobject, jmethodID, va_list) JNI_ABI;
    jfloat      (*CallFloatMethodA)(JNIEnv*, jobject, jmethodID, const jvalue*) JNI_ABI;
    jdouble     (*CallDoubleMethod)(JNIEnv*, jobject, jmethodID, ...) JNI_ABI;
    jdouble     (*CallDoubleMethodV)(JNIEnv*, jobject, jmethodID, va_list) JNI_ABI;
    jdouble     (*CallDoubleMethodA)(JNIEnv*, jobject, jmethodID, const jvalue*) JNI_ABI;
    void        (*CallVoidMethod)(JNIEnv*, jobject, jmethodID, ...) JNI_ABI;
    void        (*CallVoidMethodV)(JNIEnv*, jobject, jmethodID, va_list) JNI_ABI;
    void        (*CallVoidMethodA)(JNIEnv*, jobject, jmethodID, const jvalue*) JNI_ABI;

    jobject     (*CallNonvirtualObjectMethod)(JNIEnv*, jobject, jclass,
                        jmethodID, ...) JNI_ABI;
    jobject     (*CallNonvirtualObjectMethodV)(JNIEnv*, jobject, jclass,
                        jmethodID, va_list) JNI_ABI;
    jobject     (*CallNonvirtualObjectMethodA)(JNIEnv*, jobject, jclass,
                        jmethodID, const jvalue*) JNI_ABI;
    jboolean    (*CallNonvirtualBooleanMethod)(JNIEnv*, jobject, jclass,
                        jmethodID, ...) JNI_ABI;
    jboolean    (*CallNonvirtualBooleanMethodV)(JNIEnv*, jobject, jclass,
                         jmethodID, va_list) JNI_ABI;
    jboolean    (*CallNonvirtualBooleanMethodA)(JNIEnv*, jobject, jclass,
                         jmethodID, const jvalue*) JNI_ABI;
    jbyte       (*CallNonvirtualByteMethod)(JNIEnv*, jobject, jclass,
                        jmethodID, ...) JNI_ABI;
    jbyte       (*CallNonvirtualByteMethodV)(JNIEnv*, jobject, jclass,
                        jmethodID, va_list) JNI_ABI;
    jbyte       (*CallNonvirtualByteMethodA)(JNIEnv*, jobject, jclass,
                        jmethodID, const jvalue*) JNI_ABI;
    jchar       (*CallNonvirtualCharMethod)(JNIEnv*, jobject, jclass,
                        jmethodID, ...) JNI_ABI;
    jchar       (*CallNonvirtualCharMethodV)(JNIEnv*, jobject, jclass,
                        jmethodID, va_list) JNI_ABI;
    jchar       (*CallNonvirtualCharMethodA)(JNIEnv*, jobject, jclass,
                        jmethodID, const jvalue*) JNI_ABI;
    jshort      (*CallNonvirtualShortMethod)(JNIEnv*, jobject, jclass,
                        jmethodID, ...) JNI_ABI;
    jshort      (*CallNonvirtualShortMethodV)(JNIEnv*, jobject, jclass,
                        jmethodID, va_list) JNI_ABI;
    jshort      (*CallNonvirtualShortMethodA)(JNIEnv*, jobject, jclass,
                        jmethodID, const jvalue*) JNI_ABI;
    jint        (*CallNonvirtualIntMethod)(JNIEnv*, jobject, jclass,
                        jmethodID, ...) JNI_ABI;
    jint        (*CallNonvirtualIntMethodV)(JNIEnv*, jobject, jclass,
                        jmethodID, va_list) JNI_ABI;
    jint        (*CallNonvirtualIntMethodA)(JNIEnv*, jobject, jclass,
                        jmethodID, const jvalue*) JNI_ABI;
    jlong       (*CallNonvirtualLongMethod)(JNIEnv*, jobject, jclass,
                        jmethodID, ...) JNI_ABI;
    jlong       (*CallNonvirtualLongMethodV)(JNIEnv*, jobject, jclass,
                        jmethodID, va_list) JNI_ABI;
    jlong       (*CallNonvirtualLongMethodA)(JNIEnv*, jobject, jclass,
                        jmethodID, const jvalue*) JNI_ABI;
    jfloat      (*CallNonvirtualFloatMethod)(JNIEnv*, jobject, jclass,
                        jmethodID, ...) JNI_ABI;
    jfloat      (*CallNonvirtualFloatMethodV)(JNIEnv*, jobject, jclass,
                        jmethodID, va_list) JNI_ABI;
    jfloat      (*CallNonvirtualFloatMethodA)(JNIEnv*, jobject, jclass,
                        jmethodID, const jvalue*) JNI_ABI;
    jdouble     (*CallNonvirtualDoubleMethod)(JNIEnv*, jobject, jclass,
                        jmethodID, ...) JNI_ABI;
    jdouble     (*CallNonvirtualDoubleMethodV)(JNIEnv*, jobject, jclass,
                        jmethodID, va_list) JNI_ABI;
    jdouble     (*CallNonvirtualDoubleMethodA)(JNIEnv*, jobject, jclass,
                        jmethodID, const jvalue*) JNI_ABI;
    void        (*CallNonvirtualVoidMethod)(JNIEnv*, jobject, jclass,
                        jmethodID, ...) JNI_ABI;
    void        (*CallNonvirtualVoidMethodV)(JNIEnv*, jobject, jclass,
                        jmethodID, va_list) JNI_ABI;
    void        (*CallNonvirtualVoidMethodA)(JNIEnv*, jobject, jclass,
                        jmethodID, const jvalue*) JNI_ABI;

    jfieldID    (*GetFieldID)(JNIEnv*, jclass, const char*, const char*) JNI_ABI;

    jobject     (*GetObjectField)(JNIEnv*, jobject, jfieldID) JNI_ABI;
    jboolean    (*GetBooleanField)(JNIEnv*, jobject, jfieldID) JNI_ABI;
    jbyte       (*GetByteField)(JNIEnv*, jobject, jfieldID) JNI_ABI;
    jchar       (*GetCharField)(JNIEnv*, jobject, jfieldID) JNI_ABI;
    jshort      (*GetShortField)(JNIEnv*, jobject, jfieldID) JNI_ABI;
    jint        (*GetIntField)(JNIEnv*, jobject, jfieldID) JNI_ABI;
    jlong       (*GetLongField)(JNIEnv*, jobject, jfieldID) JNI_ABI;
    jfloat      (*GetFloatField)(JNIEnv*, jobject, jfieldID) JNI_ABI;
    jdouble     (*GetDoubleField)(JNIEnv*, jobject, jfieldID) JNI_ABI;

    void        (*SetObjectField)(JNIEnv*, jobject, jfieldID, jobject) JNI_ABI;
    void        (*SetBooleanField)(JNIEnv*, jobject, jfieldID, jboolean) JNI_ABI;
    void        (*SetByteField)(JNIEnv*, jobject, jfieldID, jbyte) JNI_ABI;
    void        (*SetCharField)(JNIEnv*, jobject, jfieldID, jchar) JNI_ABI;
    void        (*SetShortField)(JNIEnv*, jobject, jfieldID, jshort) JNI_ABI;
    void        (*SetIntField)(JNIEnv*, jobject, jfieldID, jint) JNI_ABI;
    void        (*SetLongField)(JNIEnv*, jobject, jfieldID, jlong) JNI_ABI;
    void        (*SetFloatField)(JNIEnv*, jobject, jfieldID, jfloat) JNI_ABI;
    void        (*SetDoubleField)(JNIEnv*, jobject, jfieldID, jdouble) JNI_ABI;

    jmethodID   (*GetStaticMethodID)(JNIEnv*, jclass, const char*, const char*) JNI_ABI;

    jobject     (*CallStaticObjectMethod)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    jobject     (*CallStaticObjectMethodV)(JNIEnv*, jclass, jmethodID, va_list) JNI_ABI;
    jobject     (*CallStaticObjectMethodA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;
    jboolean    (*CallStaticBooleanMethod)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    jboolean    (*CallStaticBooleanMethodV)(JNIEnv*, jclass, jmethodID,
                        va_list) JNI_ABI;
    jboolean    (*CallStaticBooleanMethodA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;
    jbyte       (*CallStaticByteMethod)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    jbyte       (*CallStaticByteMethodV)(JNIEnv*, jclass, jmethodID, va_list) JNI_ABI;
    jbyte       (*CallStaticByteMethodA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;
    jchar       (*CallStaticCharMethod)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    jchar       (*CallStaticCharMethodV)(JNIEnv*, jclass, jmethodID, va_list) JNI_ABI;
    jchar       (*CallStaticCharMethodA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;
    jshort      (*CallStaticShortMethod)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    jshort      (*CallStaticShortMethodV)(JNIEnv*, jclass, jmethodID, va_list) JNI_ABI;
    jshort      (*CallStaticShortMethodA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;
    jint        (*CallStaticIntMethod)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    jint        (*CallStaticIntMethodV)(JNIEnv*, jclass, jmethodID, va_list) JNI_ABI;
    jint        (*CallStaticIntMethodA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;
    jlong       (*CallStaticLongMethod)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    jlong       (*CallStaticLongMethodV)(JNIEnv*, jclass, jmethodID, va_list) JNI_ABI;
    jlong       (*CallStaticLongMethodA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;
    jfloat      (*CallStaticFloatMethod)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    jfloat      (*CallStaticFloatMethodV)(JNIEnv*, jclass, jmethodID, va_list) JNI_ABI;
    jfloat      (*CallStaticFloatMethodA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;
    jdouble     (*CallStaticDoubleMethod)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    jdouble     (*CallStaticDoubleMethodV)(JNIEnv*, jclass, jmethodID, va_list) JNI_ABI;
    jdouble     (*CallStaticDoubleMethodA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;
    void        (*CallStaticVoidMethod)(JNIEnv*, jclass, jmethodID, ...) JNI_ABI;
    void        (*CallStaticVoidMethodV)(JNIEnv*, jclass, jmethodID, va_list) JNI_ABI;
    void        (*CallStaticVoidMethodA)(JNIEnv*, jclass, jmethodID, const jvalue*) JNI_ABI;

    jfieldID    (*GetStaticFieldID)(JNIEnv*, jclass, const char*,
                        const char*) JNI_ABI;

    jobject     (*GetStaticObjectField)(JNIEnv*, jclass, jfieldID) JNI_ABI;
    jboolean    (*GetStaticBooleanField)(JNIEnv*, jclass, jfieldID) JNI_ABI;
    jbyte       (*GetStaticByteField)(JNIEnv*, jclass, jfieldID) JNI_ABI;
    jchar       (*GetStaticCharField)(JNIEnv*, jclass, jfieldID) JNI_ABI;
    jshort      (*GetStaticShortField)(JNIEnv*, jclass, jfieldID) JNI_ABI;
    jint        (*GetStaticIntField)(JNIEnv*, jclass, jfieldID) JNI_ABI;
    jlong       (*GetStaticLongField)(JNIEnv*, jclass, jfieldID) JNI_ABI;
    jfloat      (*GetStaticFloatField)(JNIEnv*, jclass, jfieldID) JNI_ABI;
    jdouble     (*GetStaticDoubleField)(JNIEnv*, jclass, jfieldID) JNI_ABI;

    void        (*SetStaticObjectField)(JNIEnv*, jclass, jfieldID, jobject) JNI_ABI;
    void        (*SetStaticBooleanField)(JNIEnv*, jclass, jfieldID, jboolean) JNI_ABI;
    void        (*SetStaticByteField)(JNIEnv*, jclass, jfieldID, jbyte) JNI_ABI;
    void        (*SetStaticCharField)(JNIEnv*, jclass, jfieldID, jchar) JNI_ABI;
    void        (*SetStaticShortField)(JNIEnv*, jclass, jfieldID, jshort) JNI_ABI;
    void        (*SetStaticIntField)(JNIEnv*, jclass, jfieldID, jint) JNI_ABI;
    void        (*SetStaticLongField)(JNIEnv*, jclass, jfieldID, jlong) JNI_ABI;
    void        (*SetStaticFloatField)(JNIEnv*, jclass, jfieldID, jfloat) JNI_ABI;
    void        (*SetStaticDoubleField)(JNIEnv*, jclass, jfieldID, jdouble) JNI_ABI;

    jstring     (*NewString)(JNIEnv*, const jchar*, jsize) JNI_ABI;
    jsize       (*GetStringLength)(JNIEnv*, jstring) JNI_ABI;
    const jchar* (*GetStringChars)(JNIEnv*, jstring, jboolean*) JNI_ABI;
    void        (*ReleaseStringChars)(JNIEnv*, jstring, const jchar*) JNI_ABI;
    jstring     (*NewStringUTF)(JNIEnv*, const char*) JNI_ABI;
    jsize       (*GetStringUTFLength)(JNIEnv*, jstring) JNI_ABI;
    /* JNI spec says this returns const jbyte*, but that's inconsistent */
    const char* (*GetStringUTFChars)(JNIEnv*, jstring, jboolean*) JNI_ABI;
    void        (*ReleaseStringUTFChars)(JNIEnv*, jstring, const char*) JNI_ABI;
    jsize       (*GetArrayLength)(JNIEnv*, jarray) JNI_ABI;
    jobjectArray (*NewObjectArray)(JNIEnv*, jsize, jclass, jobject) JNI_ABI;
    jobject     (*GetObjectArrayElement)(JNIEnv*, jobjectArray, jsize) JNI_ABI;
    void        (*SetObjectArrayElement)(JNIEnv*, jobjectArray, jsize, jobject) JNI_ABI;

    jbooleanArray (*NewBooleanArray)(JNIEnv*, jsize) JNI_ABI;
    jbyteArray    (*NewByteArray)(JNIEnv*, jsize) JNI_ABI;
    jcharArray    (*NewCharArray)(JNIEnv*, jsize) JNI_ABI;
    jshortArray   (*NewShortArray)(JNIEnv*, jsize) JNI_ABI;
    jintArray     (*NewIntArray)(JNIEnv*, jsize) JNI_ABI;
    jlongArray    (*NewLongArray)(JNIEnv*, jsize) JNI_ABI;
    jfloatArray   (*NewFloatArray)(JNIEnv*, jsize) JNI_ABI;
    jdoubleArray  (*NewDoubleArray)(JNIEnv*, jsize) JNI_ABI;

    jboolean*   (*GetBooleanArrayElements)(JNIEnv*, jbooleanArray, jboolean*) JNI_ABI;
    jbyte*      (*GetByteArrayElements)(JNIEnv*, jbyteArray, jboolean*) JNI_ABI;
    jchar*      (*GetCharArrayElements)(JNIEnv*, jcharArray, jboolean*) JNI_ABI;
    jshort*     (*GetShortArrayElements)(JNIEnv*, jshortArray, jboolean*) JNI_ABI;
    jint*       (*GetIntArrayElements)(JNIEnv*, jintArray, jboolean*) JNI_ABI;
    jlong*      (*GetLongArrayElements)(JNIEnv*, jlongArray, jboolean*) JNI_ABI;
    jfloat*     (*GetFloatArrayElements)(JNIEnv*, jfloatArray, jboolean*) JNI_ABI;
    jdouble*    (*GetDoubleArrayElements)(JNIEnv*, jdoubleArray, jboolean*) JNI_ABI;

    void        (*ReleaseBooleanArrayElements)(JNIEnv*, jbooleanArray,
                        jboolean*, jint) JNI_ABI;
    void        (*ReleaseByteArrayElements)(JNIEnv*, jbyteArray,
                        jbyte*, jint) JNI_ABI;
    void        (*ReleaseCharArrayElements)(JNIEnv*, jcharArray,
                        jchar*, jint) JNI_ABI;
    void        (*ReleaseShortArrayElements)(JNIEnv*, jshortArray,
                        jshort*, jint) JNI_ABI;
    void        (*ReleaseIntArrayElements)(JNIEnv*, jintArray,
                        jint*, jint) JNI_ABI;
    void        (*ReleaseLongArrayElements)(JNIEnv*, jlongArray,
                        jlong*, jint) JNI_ABI;
    void        (*ReleaseFloatArrayElements)(JNIEnv*, jfloatArray,
                        jfloat*, jint) JNI_ABI;
    void        (*ReleaseDoubleArrayElements)(JNIEnv*, jdoubleArray,
                        jdouble*, jint) JNI_ABI;

    void        (*GetBooleanArrayRegion)(JNIEnv*, jbooleanArray,
                        jsize, jsize, jboolean*) JNI_ABI;
    void        (*GetByteArrayRegion)(JNIEnv*, jbyteArray,
                        jsize, jsize, jbyte*) JNI_ABI;
    void        (*GetCharArrayRegion)(JNIEnv*, jcharArray,
                        jsize, jsize, jchar*) JNI_ABI;
    void        (*GetShortArrayRegion)(JNIEnv*, jshortArray,
                        jsize, jsize, jshort*) JNI_ABI;
    void        (*GetIntArrayRegion)(JNIEnv*, jintArray,
                        jsize, jsize, jint*) JNI_ABI;
    void        (*GetLongArrayRegion)(JNIEnv*, jlongArray,
                        jsize, jsize, jlong*) JNI_ABI;
    void        (*GetFloatArrayRegion)(JNIEnv*, jfloatArray,
                        jsize, jsize, jfloat*) JNI_ABI;
    void        (*GetDoubleArrayRegion)(JNIEnv*, jdoubleArray,
                        jsize, jsize, jdouble*) JNI_ABI;

    /* spec shows these without const; some jni.h do, some don't */
    void        (*SetBooleanArrayRegion)(JNIEnv*, jbooleanArray,
                        jsize, jsize, const jboolean*) JNI_ABI;
    void        (*SetByteArrayRegion)(JNIEnv*, jbyteArray,
                        jsize, jsize, const jbyte*) JNI_ABI;
    void        (*SetCharArrayRegion)(JNIEnv*, jcharArray,
                        jsize, jsize, const jchar*) JNI_ABI;
    void        (*SetShortArrayRegion)(JNIEnv*, jshortArray,
                        jsize, jsize, const jshort*) JNI_ABI;
    void        (*SetIntArrayRegion)(JNIEnv*, jintArray,
                        jsize, jsize, const jint*) JNI_ABI;
    void        (*SetLongArrayRegion)(JNIEnv*, jlongArray,
                        jsize, jsize, const jlong*) JNI_ABI;
    void        (*SetFloatArrayRegion)(JNIEnv*, jfloatArray,
                        jsize, jsize, const jfloat*) JNI_ABI;
    void        (*SetDoubleArrayRegion)(JNIEnv*, jdoubleArray,
                        jsize, jsize, const jdouble*) JNI_ABI;

    jint        (*RegisterNatives)(JNIEnv*, jclass, const JNINativeMethod*,
                        jint) JNI_ABI;
    jint        (*UnregisterNatives)(JNIEnv*, jclass) JNI_ABI;
    jint        (*MonitorEnter)(JNIEnv*, jobject) JNI_ABI;
    jint        (*MonitorExit)(JNIEnv*, jobject) JNI_ABI;
    jint        (*GetJavaVM)(JNIEnv*, JavaVM**) JNI_ABI;

    void        (*GetStringRegion)(JNIEnv*, jstring, jsize, jsize, jchar*) JNI_ABI;
    void        (*GetStringUTFRegion)(JNIEnv*, jstring, jsize, jsize, char*) JNI_ABI;

    void*       (*GetPrimitiveArrayCritical)(JNIEnv*, jarray, jboolean*) JNI_ABI;
    void        (*ReleasePrimitiveArrayCritical)(JNIEnv*, jarray, void*, jint) JNI_ABI;

    const jchar* (*GetStringCritical)(JNIEnv*, jstring, jboolean*) JNI_ABI;
    void        (*ReleaseStringCritical)(JNIEnv*, jstring, const jchar*) JNI_ABI;

    jweak       (*NewWeakGlobalRef)(JNIEnv*, jobject) JNI_ABI;
    void        (*DeleteWeakGlobalRef)(JNIEnv*, jweak) JNI_ABI;

    jboolean    (*ExceptionCheck)(JNIEnv*) JNI_ABI;

    jobject     (*NewDirectByteBuffer)(JNIEnv*, void*, jlong) JNI_ABI;
    void*       (*GetDirectBufferAddress)(JNIEnv*, jobject) JNI_ABI;
    jlong       (*GetDirectBufferCapacity)(JNIEnv*, jobject) JNI_ABI;

    /* added in JNI 1.6 */
    jobjectRefType (*GetObjectRefType)(JNIEnv*, jobject) JNI_ABI;
};

/*
 * C++ object wrapper.
 *
 * This is usually overlaid on a C struct whose first element is a
 * JNINativeInterface*.  We rely somewhat on compiler behavior.
 */
struct _JNIEnv {
    /* do not rename this; it does not seem to be entirely opaque */
    const struct JNINativeInterface* functions;

#if defined(__cplusplus)

    jint GetVersion()
    { return functions->GetVersion(this); }

    jclass DefineClass(const char *name, jobject loader, const jbyte* buf,
        jsize bufLen)
    { return functions->DefineClass(this, name, loader, buf, bufLen); }

    jclass FindClass(const char* name)
    { return functions->FindClass(this, name); }

    jmethodID FromReflectedMethod(jobject method)
    { return functions->FromReflectedMethod(this, method); }

    jfieldID FromReflectedField(jobject field)
    { return functions->FromReflectedField(this, field); }

    jobject ToReflectedMethod(jclass cls, jmethodID methodID, jboolean isStatic)
    { return functions->ToReflectedMethod(this, cls, methodID, isStatic); }

    jclass GetSuperclass(jclass clazz)
    { return functions->GetSuperclass(this, clazz); }

    jboolean IsAssignableFrom(jclass clazz1, jclass clazz2)
    { return functions->IsAssignableFrom(this, clazz1, clazz2); }

    jobject ToReflectedField(jclass cls, jfieldID fieldID, jboolean isStatic)
    { return functions->ToReflectedField(this, cls, fieldID, isStatic); }

    jint Throw(jthrowable obj)
    { return functions->Throw(this, obj); }

    jint ThrowNew(jclass clazz, const char* message)
    { return functions->ThrowNew(this, clazz, message); }

    jthrowable ExceptionOccurred()
    { return functions->ExceptionOccurred(this); }

    void ExceptionDescribe()
    { functions->ExceptionDescribe(this); }

    void ExceptionClear()
    { functions->ExceptionClear(this); }

    void FatalError(const char* msg)
    { functions->FatalError(this, msg); }

    jint PushLocalFrame(jint capacity)
    { return functions->PushLocalFrame(this, capacity); }

    jobject PopLocalFrame(jobject result)
    { return functions->PopLocalFrame(this, result); }

    jobject NewGlobalRef(jobject obj)
    { return functions->NewGlobalRef(this, obj); }

    void DeleteGlobalRef(jobject globalRef)
    { functions->DeleteGlobalRef(this, globalRef); }

    void DeleteLocalRef(jobject localRef)
    { functions->DeleteLocalRef(this, localRef); }

    jboolean IsSameObject(jobject ref1, jobject ref2)
    { return functions->IsSameObject(this, ref1, ref2); }

    jobject NewLocalRef(jobject ref)
    { return functions->NewLocalRef(this, ref); }

    jint EnsureLocalCapacity(jint capacity)
    { return functions->EnsureLocalCapacity(this, capacity); }

    jobject AllocObject(jclass clazz)
    { return functions->AllocObject(this, clazz); }

    jobject NewObject(jclass clazz, jmethodID methodID, ...)
    {
        va_list args;
        va_start(args, methodID);
        jobject result = functions->NewObjectV(this, clazz, methodID, args);
        va_end(args);
        return result;
    }

    jobject NewObjectV(jclass clazz, jmethodID methodID, va_list args)
    { return functions->NewObjectV(this, clazz, methodID, args); }

    jobject NewObjectA(jclass clazz, jmethodID methodID, const jvalue* args)
    { return functions->NewObjectA(this, clazz, methodID, args); }

    jclass GetObjectClass(jobject obj)
    { return functions->GetObjectClass(this, obj); }

    jboolean IsInstanceOf(jobject obj, jclass clazz)
    { return functions->IsInstanceOf(this, obj, clazz); }

    jmethodID GetMethodID(jclass clazz, const char* name, const char* sig)
    { return functions->GetMethodID(this, clazz, name, sig); }

#define CALL_TYPE_METHOD(_jtype, _jname)                                    \
    _jtype Call##_jname##Method(jobject obj, jmethodID methodID, ...)       \
    {                                                                       \
        _jtype result;                                                      \
        va_list args;                                                       \
        va_start(args, methodID);                                           \
        result = functions->Call##_jname##MethodV(this, obj, methodID,      \
                    args);                                                  \
        va_end(args);                                                       \
        return result;                                                      \
    }
#define CALL_TYPE_METHODV(_jtype, _jname)                                   \
    _jtype Call##_jname##MethodV(jobject obj, jmethodID methodID,           \
        va_list args)                                                       \
    { return functions->Call##_jname##MethodV(this, obj, methodID, args); }
#define CALL_TYPE_METHODA(_jtype, _jname)                                   \
    _jtype Call##_jname##MethodA(jobject obj, jmethodID methodID,           \
                                 const jvalue* args)                        \
    { return functions->Call##_jname##MethodA(this, obj, methodID, args); }

#define CALL_TYPE(_jtype, _jname)                                           \
    CALL_TYPE_METHOD(_jtype, _jname)                                        \
    CALL_TYPE_METHODV(_jtype, _jname)                                       \
    CALL_TYPE_METHODA(_jtype, _jname)

    CALL_TYPE(jobject, Object)
    CALL_TYPE(jboolean, Boolean)
    CALL_TYPE(jbyte, Byte)
    CALL_TYPE(jchar, Char)
    CALL_TYPE(jshort, Short)
    CALL_TYPE(jint, Int)
    CALL_TYPE(jlong, Long)
    CALL_TYPE(jfloat, Float)
    CALL_TYPE(jdouble, Double)

    void CallVoidMethod(jobject obj, jmethodID methodID, ...)
    {
        va_list args;
        va_start(args, methodID);
        functions->CallVoidMethodV(this, obj, methodID, args);
        va_end(args);
    }
    void CallVoidMethodV(jobject obj, jmethodID methodID, va_list args)
    { functions->CallVoidMethodV(this, obj, methodID, args); }
    void CallVoidMethodA(jobject obj, jmethodID methodID, const jvalue* args)
    { functions->CallVoidMethodA(this, obj, methodID, args); }

#define CALL_NONVIRT_TYPE_METHOD(_jtype, _jname)                            \
    _jtype CallNonvirtual##_jname##Method(jobject obj, jclass clazz,        \
        jmethodID methodID, ...)                                            \
    {                                                                       \
        _jtype result;                                                      \
        va_list args;                                                       \
        va_start(args, methodID);                                           \
        result = functions->CallNonvirtual##_jname##MethodV(this, obj,      \
                    clazz, methodID, args);                                 \
        va_end(args);                                                       \
        return result;                                                      \
    }
#define CALL_NONVIRT_TYPE_METHODV(_jtype, _jname)                           \
    _jtype CallNonvirtual##_jname##MethodV(jobject obj, jclass clazz,       \
        jmethodID methodID, va_list args)                                   \
    { return functions->CallNonvirtual##_jname##MethodV(this, obj, clazz,   \
        methodID, args); }
#define CALL_NONVIRT_TYPE_METHODA(_jtype, _jname)                           \
    _jtype CallNonvirtual##_jname##MethodA(jobject obj, jclass clazz,       \
        jmethodID methodID, const jvalue* args)                             \
    { return functions->CallNonvirtual##_jname##MethodA(this, obj, clazz,   \
        methodID, args); }

#define CALL_NONVIRT_TYPE(_jtype, _jname)                                   \
    CALL_NONVIRT_TYPE_METHOD(_jtype, _jname)                                \
    CALL_NONVIRT_TYPE_METHODV(_jtype, _jname)                               \
    CALL_NONVIRT_TYPE_METHODA(_jtype, _jname)

    CALL_NONVIRT_TYPE(jobject, Object)
    CALL_NONVIRT_TYPE(jboolean, Boolean)
    CALL_NONVIRT_TYPE(jbyte, Byte)
    CALL_NONVIRT_TYPE(jchar, Char)
    CALL_NONVIRT_TYPE(jshort, Short)
    CALL_NONVIRT_TYPE(jint, Int)
    CALL_NONVIRT_TYPE(jlong, Long)
    CALL_NONVIRT_TYPE(jfloat, Float)
    CALL_NONVIRT_TYPE(jdouble, Double)

    void CallNonvirtualVoidMethod(jobject obj, jclass clazz,
        jmethodID methodID, ...)
    {
        va_list args;
        va_start(args, methodID);
        functions->CallNonvirtualVoidMethodV(this, obj, clazz, methodID, args);
        va_end(args);
    }
    void CallNonvirtualVoidMethodV(jobject obj, jclass clazz,
        jmethodID methodID, va_list args)
    { functions->CallNonvirtualVoidMethodV(this, obj, clazz, methodID, args); }
    void CallNonvirtualVoidMethodA(jobject obj, jclass clazz,
        jmethodID methodID, const jvalue* args)
    { functions->CallNonvirtualVoidMethodA(this, obj, clazz, methodID, args); }

    jfieldID GetFieldID(jclass clazz, const char* name, const char* sig)
    { return functions->GetFieldID(this, clazz, name, sig); }

    jobject GetObjectField(jobject obj, jfieldID fieldID)
    { return functions->GetObjectField(this, obj, fieldID); }
    jboolean GetBooleanField(jobject obj, jfieldID fieldID)
    { return functions->GetBooleanField(this, obj, fieldID); }
    jbyte GetByteField(jobject obj, jfieldID fieldID)
    { return functions->GetByteField(this, obj, fieldID); }
    jchar GetCharField(jobject obj, jfieldID fieldID)
    { return functions->GetCharField(this, obj, fieldID); }
    jshort GetShortField(jobject obj, jfieldID fieldID)
    { return functions->GetShortField(this, obj, fieldID); }
    jint GetIntField(jobject obj, jfieldID fieldID)
    { return functions->GetIntField(this, obj, fieldID); }
    jlong GetLongField(jobject obj, jfieldID fieldID)
    { return functions->GetLongField(this, obj, fieldID); }
    jfloat GetFloatField(jobject obj, jfieldID fieldID)
    { return functions->GetFloatField(this, obj, fieldID); }
    jdouble GetDoubleField(jobject obj, jfieldID fieldID)
    { return functions->GetDoubleField(this, obj, fieldID); }

    void SetObjectField(jobject obj, jfieldID fieldID, jobject value)
    { functions->SetObjectField(this, obj, fieldID, value); }
    void SetBooleanField(jobject obj, jfieldID fieldID, jboolean value)
    { functions->SetBooleanField(this, obj, fieldID, value); }
    void SetByteField(jobject obj, jfieldID fieldID, jbyte value)
    { functions->SetByteField(this, obj, fieldID, value); }
    void SetCharField(jobject obj, jfieldID fieldID, jchar value)
    { functions->SetCharField(this, obj, fieldID, value); }
    void SetShortField(jobject obj, jfieldID fieldID, jshort value)
    { functions->SetShortField(this, obj, fieldID, value); }
    void SetIntField(jobject obj, jfieldID fieldID, jint value)
    { functions->SetIntField(this, obj, fieldID, value); }
    void SetLongField(jobject obj, jfieldID fieldID, jlong value)
    { functions->SetLongField(this, obj, fieldID, value); }
    void SetFloatField(jobject obj, jfieldID fieldID, jfloat value)
    { functions->SetFloatField(this, obj, fieldID, value); }
    void SetDoubleField(jobject obj, jfieldID fieldID, jdouble value)
    { functions->SetDoubleField(this, obj, fieldID, value); }

    jmethodID GetStaticMethodID(jclass clazz, const char* name, const char* sig)
    { return functions->GetStaticMethodID(this, clazz, name, sig); }

#define CALL_STATIC_TYPE_METHOD(_jtype, _jname)                             \
    _jtype CallStatic##_jname##Method(jclass clazz, jmethodID methodID,     \
        ...)                                                                \
    {                                                                       \
        _jtype result;                                                      \
        va_list args;                                                       \
        va_start(args, methodID);                                           \
        result = functions->CallStatic##_jname##MethodV(this, clazz,        \
                    methodID, args);                                        \
        va_end(args);                                                       \
        return result;                                                      \
    }
#define CALL_STATIC_TYPE_METHODV(_jtype, _jname)                            \
    _jtype CallStatic##_jname##MethodV(jclass clazz, jmethodID methodID,    \
        va_list args)                                                       \
    { return functions->CallStatic##_jname##MethodV(this, clazz, methodID,  \
        args); }
#define CALL_STATIC_TYPE_METHODA(_jtype, _jname)                            \
    _jtype CallStatic##_jname##MethodA(jclass clazz, jmethodID methodID,    \
                                       const jvalue* args)                  \
    { return functions->CallStatic##_jname##MethodA(this, clazz, methodID,  \
        args); }

#define CALL_STATIC_TYPE(_jtype, _jname)                                    \
    CALL_STATIC_TYPE_METHOD(_jtype, _jname)                                 \
    CALL_STATIC_TYPE_METHODV(_jtype, _jname)                                \
    CALL_STATIC_TYPE_METHODA(_jtype, _jname)

    CALL_STATIC_TYPE(jobject, Object)
    CALL_STATIC_TYPE(jboolean, Boolean)
    CALL_STATIC_TYPE(jbyte, Byte)
    CALL_STATIC_TYPE(jchar, Char)
    CALL_STATIC_TYPE(jshort, Short)
    CALL_STATIC_TYPE(jint, Int)
    CALL_STATIC_TYPE(jlong, Long)
    CALL_STATIC_TYPE(jfloat, Float)
    CALL_STATIC_TYPE(jdouble, Double)

    void CallStaticVoidMethod(jclass clazz, jmethodID methodID, ...)
    {
        va_list args;
        va_start(args, methodID);
        functions->CallStaticVoidMethodV(this, clazz, methodID, args);
        va_end(args);
    }
    void CallStaticVoidMethodV(jclass clazz, jmethodID methodID, va_list args)
    { functions->CallStaticVoidMethodV(this, clazz, methodID, args); }
    void CallStaticVoidMethodA(jclass clazz, jmethodID methodID, const jvalue* args)
    { functions->CallStaticVoidMethodA(this, clazz, methodID, args); }

    jfieldID GetStaticFieldID(jclass clazz, const char* name, const char* sig)
    { return functions->GetStaticFieldID(this, clazz, name, sig); }

    jobject GetStaticObjectField(jclass clazz, jfieldID fieldID)
    { return functions->GetStaticObjectField(this, clazz, fieldID); }
    jboolean GetStaticBooleanField(jclass clazz, jfieldID fieldID)
    { return functions->GetStaticBooleanField(this, clazz, fieldID); }
    jbyte GetStaticByteField(jclass clazz, jfieldID fieldID)
    { return functions->GetStaticByteField(this, clazz, fieldID); }
    jchar GetStaticCharField(jclass clazz, jfieldID fieldID)
    { return functions->GetStaticCharField(this, clazz, fieldID); }
    jshort GetStaticShortField(jclass clazz, jfieldID fieldID)
    { return functions->GetStaticShortField(this, clazz, fieldID); }
    jint GetStaticIntField(jclass clazz, jfieldID fieldID)
    { return functions->GetStaticIntField(this, clazz, fieldID); }
    jlong GetStaticLongField(jclass clazz, jfieldID fieldID)
    { return functions->GetStaticLongField(this, clazz, fieldID); }
    jfloat GetStaticFloatField(jclass clazz, jfieldID fieldID)
    { return functions->GetStaticFloatField(this, clazz, fieldID); }
    jdouble GetStaticDoubleField(jclass clazz, jfieldID fieldID)
    { return functions->GetStaticDoubleField(this, clazz, fieldID); }

    void SetStaticObjectField(jclass clazz, jfieldID fieldID, jobject value)
    { functions->SetStaticObjectField(this, clazz, fieldID, value); }
    void SetStaticBooleanField(jclass clazz, jfieldID fieldID, jboolean value)
    { functions->SetStaticBooleanField(this, clazz, fieldID, value); }
    void SetStaticByteField(jclass clazz, jfieldID fieldID, jbyte value)
    { functions->SetStaticByteField(this, clazz, fieldID, value); }
    void SetStaticCharField(jclass clazz, jfieldID fieldID, jchar value)
    { functions->SetStaticCharField(this, clazz, fieldID, value); }
    void SetStaticShortField(jclass clazz, jfieldID fieldID, jshort value)
    { functions->SetStaticShortField(this, clazz, fieldID, value); }
    void SetStaticIntField(jclass clazz, jfieldID fieldID, jint value)
    { functions->SetStaticIntField(this, clazz, fieldID, value); }
    void SetStaticLongField(jclass clazz, jfieldID fieldID, jlong value)
    { functions->SetStaticLongField(this, clazz, fieldID, value); }
    void SetStaticFloatField(jclass clazz, jfieldID fieldID, jfloat value)
    { functions->SetStaticFloatField(this, clazz, fieldID, value); }
    void SetStaticDoubleField(jclass clazz, jfieldID fieldID, jdouble value)
    { functions->SetStaticDoubleField(this, clazz, fieldID, value); }

    jstring NewString(const jchar* unicodeChars, jsize len)
    { return functions->NewString(this, unicodeChars, len); }

    jsize GetStringLength(jstring string)
    { return functions->GetStringLength(this, string); }

    const jchar* GetStringChars(jstring string, jboolean* isCopy)
    { return functions->GetStringChars(this, string, isCopy); }

    void ReleaseStringChars(jstring string, const jchar* chars)
    { functions->ReleaseStringChars(this, string, chars); }

    jstring NewStringUTF(const char* bytes)
    { return functions->NewStringUTF(this, bytes); }

    jsize GetStringUTFLength(jstring string)
    { return functions->GetStringUTFLength(this, string); }

    const char* GetStringUTFChars(jstring string, jboolean* isCopy)
    { return functions->GetStringUTFChars(this, string, isCopy); }

    void ReleaseStringUTFChars(jstring string, const char* utf)
    { functions->ReleaseStringUTFChars(this, string, utf); }

    jsize GetArrayLength(jarray array)
    { return functions->GetArrayLength(this, array); }

    jobjectArray NewObjectArray(jsize length, jclass elementClass,
        jobject initialElement)
    { return functions->NewObjectArray(this, length, elementClass,
        initialElement); }

    jobject GetObjectArrayElement(jobjectArray array, jsize index)
    { return functions->GetObjectArrayElement(this, array, index); }

    void SetObjectArrayElement(jobjectArray array, jsize index, jobject value)
    { functions->SetObjectArrayElement(this, array, index, value); }

    jbooleanArray NewBooleanArray(jsize length)
    { return functions->NewBooleanArray(this, length); }
    jbyteArray NewByteArray(jsize length)
    { return functions->NewByteArray(this, length); }
    jcharArray NewCharArray(jsize length)
    { return functions->NewCharArray(this, length); }
    jshortArray NewShortArray(jsize length)
    { return functions->NewShortArray(this, length); }
    jintArray NewIntArray(jsize length)
    { return functions->NewIntArray(this, length); }
    jlongArray NewLongArray(jsize length)
    { return functions->NewLongArray(this, length); }
    jfloatArray NewFloatArray(jsize length)
    { return functions->NewFloatArray(this, length); }
    jdoubleArray NewDoubleArray(jsize length)
    { return functions->NewDoubleArray(this, length); }

    jboolean* GetBooleanArrayElements(jbooleanArray array, jboolean* isCopy)
    { return functions->GetBooleanArrayElements(this, array, isCopy); }
    jbyte* GetByteArrayElements(jbyteArray array, jboolean* isCopy)
    { return functions->GetByteArrayElements(this, array, isCopy); }
    jchar* GetCharArrayElements(jcharArray array, jboolean* isCopy)
    { return functions->GetCharArrayElements(this, array, isCopy); }
    jshort* GetShortArrayElements(jshortArray array, jboolean* isCopy)
    { return functions->GetShortArrayElements(this, array, isCopy); }
    jint* GetIntArrayElements(jintArray array, jboolean* isCopy)
    { return functions->GetIntArrayElements(this, array, isCopy); }
    jlong* GetLongArrayElements(jlongArray array, jboolean* isCopy)
    { return functions->GetLongArrayElements(this, array, isCopy); }
    jfloat* GetFloatArrayElements(jfloatArray array, jboolean* isCopy)
    { return functions->GetFloatArrayElements(this, array, isCopy); }
    jdouble* GetDoubleArrayElements(jdoubleArray array, jboolean* isCopy)
    { return functions->GetDoubleArrayElements(this, array, isCopy); }

    void ReleaseBooleanArrayElements(jbooleanArray array, jboolean* elems,
        jint mode)
    { functions->ReleaseBooleanArrayElements(this, array, elems, mode); }
    void ReleaseByteArrayElements(jbyteArray array, jbyte* elems,
        jint mode)
    { functions->ReleaseByteArrayElements(this, array, elems, mode); }
    void ReleaseCharArrayElements(jcharArray array, jchar* elems,
        jint mode)
    { functions->ReleaseCharArrayElements(this, array, elems, mode); }
    void ReleaseShortArrayElements(jshortArray array, jshort* elems,
        jint mode)
    { functions->ReleaseShortArrayElements(this, array, elems, mode); }
    void ReleaseIntArrayElements(jintArray array, jint* elems,
        jint mode)
    { functions->ReleaseIntArrayElements(this, array, elems, mode); }
    void ReleaseLongArrayElements(jlongArray array, jlong* elems,
        jint mode)
    { functions->ReleaseLongArrayElements(this, array, elems, mode); }
    void ReleaseFloatArrayElements(jfloatArray array, jfloat* elems,
        jint mode)
    { functions->ReleaseFloatArrayElements(this, array, elems, mode); }
    void ReleaseDoubleArrayElements(jdoubleArray array, jdouble* elems,
        jint mode)
    { functions->ReleaseDoubleArrayElements(this, array, elems, mode); }

    void GetBooleanArrayRegion(jbooleanArray array, jsize start, jsize len,
        jboolean* buf)
    { functions->GetBooleanArrayRegion(this, array, start, len, buf); }
    void GetByteArrayRegion(jbyteArray array, jsize start, jsize len,
        jbyte* buf)
    { functions->GetByteArrayRegion(this, array, start, len, buf); }
    void GetCharArrayRegion(jcharArray array, jsize start, jsize len,
        jchar* buf)
    { functions->GetCharArrayRegion(this, array, start, len, buf); }
    void GetShortArrayRegion(jshortArray array, jsize start, jsize len,
        jshort* buf)
    { functions->GetShortArrayRegion(this, array, start, len, buf); }
    void GetIntArrayRegion(jintArray array, jsize start, jsize len,
        jint* buf)
    { functions->GetIntArrayRegion(this, array, start, len, buf); }
    void GetLongArrayRegion(jlongArray array, jsize start, jsize len,
        jlong* buf)
    { functions->GetLongArrayRegion(this, array, start, len, buf); }
    void GetFloatArrayRegion(jfloatArray array, jsize start, jsize len,
        jfloat* buf)
    { functions->GetFloatArrayRegion(this, array, start, len, buf); }
    void GetDoubleArrayRegion(jdoubleArray array, jsize start, jsize len,
        jdouble* buf)
    { functions->GetDoubleArrayRegion(this, array, start, len, buf); }

    void SetBooleanArrayRegion(jbooleanArray array, jsize start, jsize len,
        const jboolean* buf)
    { functions->SetBooleanArrayRegion(this, array, start, len, buf); }
    void SetByteArrayRegion(jbyteArray array, jsize start, jsize len,
        const jbyte* buf)
    { functions->SetByteArrayRegion(this, array, start, len, buf); }
    void SetCharArrayRegion(jcharArray array, jsize start, jsize len,
        const jchar* buf)
    { functions->SetCharArrayRegion(this, array, start, len, buf); }
    void SetShortArrayRegion(jshortArray array, jsize start, jsize len,
        const jshort* buf)
    { functions->SetShortArrayRegion(this, array, start, len, buf); }
    void SetIntArrayRegion(jintArray array, jsize start, jsize len,
        const jint* buf)
    { functions->SetIntArrayRegion(this, array, start, len, buf); }
    void SetLongArrayRegion(jlongArray array, jsize start, jsize len,
        const jlong* buf)
    { functions->SetLongArrayRegion(this, array, start, len, buf); }
    void SetFloatArrayRegion(jfloatArray array, jsize start, jsize len,
        const jfloat* buf)
    { functions->SetFloatArrayRegion(this, array, start, len, buf); }
    void SetDoubleArrayRegion(jdoubleArray array, jsize start, jsize len,
        const jdouble* buf)
    { functions->SetDoubleArrayRegion(this, array, start, len, buf); }

    jint RegisterNatives(jclass clazz, const JNINativeMethod* methods,
        jint nMethods)
    { return functions->RegisterNatives(this, clazz, methods, nMethods); }

    jint UnregisterNatives(jclass clazz)
    { return functions->UnregisterNatives(this, clazz); }

    jint MonitorEnter(jobject obj)
    { return functions->MonitorEnter(this, obj); }

    jint MonitorExit(jobject obj)
    { return functions->MonitorExit(this, obj); }

    jint GetJavaVM(JavaVM** vm)
    { return functions->GetJavaVM(this, vm); }

    void GetStringRegion(jstring str, jsize start, jsize len, jchar* buf)
    { functions->GetStringRegion(this, str, start, len, buf); }

    void GetStringUTFRegion(jstring str, jsize start, jsize len, char* buf)
    { return functions->GetStringUTFRegion(this, str, start, len, buf); }

    void* GetPrimitiveArrayCritical(jarray array, jboolean* isCopy)
    { return functions->GetPrimitiveArrayCritical(this, array, isCopy); }

    void ReleasePrimitiveArrayCritical(jarray array, void* carray, jint mode)
    { functions->ReleasePrimitiveArrayCritical(this, array, carray, mode); }

    const jchar* GetStringCritical(jstring string, jboolean* isCopy)
    { return functions->GetStringCritical(this, string, isCopy); }

    void ReleaseStringCritical(jstring string, const jchar* carray)
    { functions->ReleaseStringCritical(this, string, carray); }

    jweak NewWeakGlobalRef(jobject obj)
    { return functions->NewWeakGlobalRef(this, obj); }

    void DeleteWeakGlobalRef(jweak obj)
    { functions->DeleteWeakGlobalRef(this, obj); }

    jboolean ExceptionCheck()
    { return functions->ExceptionCheck(this); }

    jobject NewDirectByteBuffer(void* address, jlong capacity)
    { return functions->NewDirectByteBuffer(this, address, capacity); }

    void* GetDirectBufferAddress(jobject buf)
    { return functions->GetDirectBufferAddress(this, buf); }

    jlong GetDirectBufferCapacity(jobject buf)
    { return functions->GetDirectBufferCapacity(this, buf); }

    /* added in JNI 1.6 */
    jobjectRefType GetObjectRefType(jobject obj)
    { return functions->GetObjectRefType(this, obj); }
#endif /*__cplusplus*/
};


/*
 * JNI invocation interface.
 */
struct JNIInvokeInterface {
    void*       reserved0;
    void*       reserved1;
    void*       reserved2;

    jint        (*DestroyJavaVM)(JavaVM*) JNI_ABI;
    jint        (*AttachCurrentThread)(JavaVM*, JNIEnv**, void*) JNI_ABI;
    jint        (*DetachCurrentThread)(JavaVM*) JNI_ABI;
    jint        (*GetEnv)(JavaVM*, void**, jint) JNI_ABI;
    jint        (*AttachCurrentThreadAsDaemon)(JavaVM*, JNIEnv**, void*) JNI_ABI;
};

/*
 * C++ version.
 */
struct _JavaVM {
    const struct JNIInvokeInterface* functions;

#if defined(__cplusplus)
    jint DestroyJavaVM()
    { return functions->DestroyJavaVM(this); }
    jint AttachCurrentThread(JNIEnv** p_env, void* thr_args)
    { return functions->AttachCurrentThread(this, p_env, thr_args); }
    jint DetachCurrentThread()
    { return functions->DetachCurrentThread(this); }
    jint GetEnv(void** env, jint version)
    { return functions->GetEnv(this, env, version); }
    jint AttachCurrentThreadAsDaemon(JNIEnv** p_env, void* thr_args)
    { return functions->AttachCurrentThreadAsDaemon(this, p_env, thr_args); }
#endif /*__cplusplus*/
};

struct JavaVMAttachArgs {
    jint        version;    /* must be >= JNI_VERSION_1_2 */
    const char* name;       /* NULL or name of thread as modified UTF-8 str */
    jobject     group;      /* global ref of a ThreadGroup object, or NULL */
};
typedef struct JavaVMAttachArgs JavaVMAttachArgs;

/*
 * JNI 1.2+ initialization.  (As of 1.6, the pre-1.2 structures are no
 * longer supported.)
 */
typedef struct JavaVMOption {
    const char* optionString;
    void*       extraInfo;
} JavaVMOption;

typedef struct JavaVMInitArgs {
    jint        version;    /* use JNI_VERSION_1_2 or later */

    jint        nOptions;
    JavaVMOption* options;
    jboolean    ignoreUnrecognized;
} JavaVMInitArgs;

#ifdef __cplusplus
extern "C" {
#endif
/*
 * VM initialization functions.
 *
 * Note these are the only symbols exported for JNI by the VM.
 */
jint JNI_GetDefaultJavaVMInitArgs(void*);
jint JNI_CreateJavaVM(JavaVM**, JNIEnv**, void*);
jint JNI_GetCreatedJavaVMs(JavaVM**, jsize, jsize*);

#define JNIIMPORT
#define JNIEXPORT  __attribute__ ((visibility ("default")))
#define JNICALL

/*
 * Prototypes for functions exported by loadable shared libs.  These are
 * called by JNI, not provided by JNI.
 */
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved);
JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved);

#ifdef __cplusplus
}
#endif


/*
 * Manifest constants.
 */
#define JNI_FALSE   0
#define JNI_TRUE    1

#define JNI_VERSION_1_1 0x00010001
#define JNI_VERSION_1_2 0x00010002
#define JNI_VERSION_1_4 0x00010004
#define JNI_VERSION_1_6 0x00010006

#define JNI_OK          (0)         /* no error */
#define JNI_ERR         (-1)        /* generic error */
#define JNI_EDETACHED   (-2)        /* thread detached from the VM */
#define JNI_EVERSION    (-3)        /* JNI version error */
#define JNI_ENOMEM      (-4)        /* Out of memory */
#define JNI_EEXIST      (-5)        /* VM already created */
#define JNI_EINVAL      (-6)        /* Invalid argument */

#define JNI_COMMIT      1           /* copy content, do not free buffer */
#define JNI_ABORT       2           /* free buffer w/o copying back */

#endif  /* JNI_H_ */
