#ifndef HV_EXPORT_H_
#define HV_EXPORT_H_

// HV_EXPORT
#if defined(HV_STATICLIB) || defined(HV_SOURCE)
    #define HV_EXPORT
#elif defined(_MSC_VER)
    #if defined(HV_DYNAMICLIB) || defined(HV_EXPORTS) || defined(hv_EXPORTS)
        #define HV_EXPORT  __declspec(dllexport)
    #else
        #define HV_EXPORT  __declspec(dllimport)
    #endif
#elif defined(__GNUC__)
    #define HV_EXPORT  __attribute__((visibility("default")))
#else
    #define HV_EXPORT
#endif

// HV_INLINE
#define HV_INLINE static inline

// HV_DEPRECATED
#if defined(HV_NO_DEPRECATED)
#define HV_DEPRECATED
#elif defined(__GNUC__) || defined(__clang__)
#define HV_DEPRECATED   __attribute__((deprecated))
#elif defined(_MSC_VER)
#define HV_DEPRECATED   __declspec(deprecated)
#else
#define HV_DEPRECATED
#endif

// HV_UNUSED
#if defined(__GNUC__)
    #define HV_UNUSED   __attribute__((unused))
#else
    #define HV_UNUSED
#endif

#ifdef __cplusplus

#ifndef EXTERN_C
#define EXTERN_C            extern "C"
#endif

#ifndef BEGIN_EXTERN_C
#define BEGIN_EXTERN_C      extern "C" {
#endif

#ifndef END_EXTERN_C
#define END_EXTERN_C        } // extern "C"
#endif

#ifndef BEGIN_NAMESPACE
#define BEGIN_NAMESPACE(ns) namespace ns {
#endif

#ifndef END_NAMESPACE
#define END_NAMESPACE(ns)   } // namespace ns
#endif

#ifndef USING_NAMESPACE
#define USING_NAMESPACE(ns) using namespace ns;
#endif

#ifndef DEFAULT
#define DEFAULT(x)  = x
#endif

#ifndef ENUM
#define ENUM(e)     enum e
#endif

#ifndef STRUCT
#define STRUCT(s)   struct s
#endif

#else

#define EXTERN_C    extern
#define BEGIN_EXTERN_C
#define END_EXTERN_C

#define BEGIN_NAMESPACE(ns)
#define END_NAMESPACE(ns)
#define USING_NAMESPACE(ns)

#ifndef DEFAULT
#define DEFAULT(x)
#endif

#ifndef ENUM
#define ENUM(e)\
typedef enum e e;\
enum e
#endif

#ifndef STRUCT
#define STRUCT(s)\
typedef struct s s;\
struct s
#endif

#endif // __cplusplus

#define BEGIN_NAMESPACE_HV  BEGIN_NAMESPACE(hv)
#define END_NAMESPACE_HV    END_NAMESPACE(hv)
#define USING_NAMESPACE_HV  USING_NAMESPACE(hv)

// MSVC ports
#ifdef _MSC_VER

#pragma warning (disable: 4251) // STL dll
#pragma warning (disable: 4275) // dll-interface

#if _MSC_VER < 1900 // < VS2015

#ifndef __cplusplus
#ifndef inline
#define inline __inline
#endif
#endif

/*
Beginning with the UCRT in Visual Studio 2015 and Windows 10, snprintf is no longer identical to _snprintf. 
The snprintf behavior is now C99 standard conformant. The difference is that if you run out of buffer, 
snprintf null-terminates the end of the buffer and returns the number of characters that would have been 
required whereas _snprintf doesn't null-terminate the buffer and returns -1. 
Also, snprintf() includes one more character in the output because it doesn't null-terminate the buffer.
*/
#ifndef snprintf
#define snprintf _snprintf
#endif

#if _MSC_VER < 1310 // < VS2005
#define __FUNCTION__ ""
#endif

#endif // < VS2015
#endif //_MSC_VER

#endif // HV_EXPORT_H_
