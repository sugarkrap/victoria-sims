#ifndef VICTORIA_CORE_TYPES_HEADER
#define VICTORIA_CORE_TYPES_HEADER

typedef signed char Integer8;
typedef unsigned char Unsigned8;
typedef signed short Integer16;
typedef unsigned short Unsigned16;
typedef signed int Integer32;
typedef unsigned int Unsigned32;
typedef signed long long Integer64;
typedef unsigned long long Unsigned64;
typedef float Real32;

typedef unsigned long MemorySize;

typedef Unsigned8 Boolean;

#define BOOLEAN_TRUE ((Boolean)1)
#define BOOLEAN_FALSE ((Boolean)0)

#define NULL_POINTER ((void *)0)

#define VICTORIA_ARRAY_LENGTH(fixedArray) (sizeof(fixedArray) / sizeof((fixedArray)[0]))

#endif
