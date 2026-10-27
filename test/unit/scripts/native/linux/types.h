#ifndef KVC_LINUX_TYPES_H
#define KVC_LINUX_TYPES_H
#include <stdint.h>
typedef uint8_t __u8;
typedef int8_t __s8;
typedef uint16_t __u16;
typedef int16_t __s16;
typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint64_t __u64;
typedef int64_t __s64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
#define __aligned_u64 __u64 __attribute__((aligned(8)))
#define __aligned_s64 __s64 __attribute__((aligned(8)))
#define __aligned_be64 __be64 __attribute__((aligned(8)))
#define __aligned_le64 __le64 __attribute__((aligned(8)))
#endif
