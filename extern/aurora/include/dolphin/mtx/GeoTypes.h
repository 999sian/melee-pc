#ifndef _DOLPHIN_GEOTYPES
#define _DOLPHIN_GEOTYPES

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

  typedef struct {
    f32 x, y;
  } Vec2, *Vec2Ptr, Point2d, *Point2dPtr;

  typedef struct Vec {
    f32 x, y, z;
  } Vec, Vec3, *VecPtr, Point3d, *Point3dPtr;

  typedef struct {
    s8 x, y, z;
  } S8Vec3, S8Vec, *S8Vec3Ptr, *S8VecPtr;

  typedef struct {
    u8 x, y, z, w;
  } U8Vec4, *U8Vec4Ptr;

  typedef struct {
    s16 x, y, z;
  } S16Vec, S16Vec3, *S16VecPtr, *S16Vec3Ptr;

  typedef struct {
    int x, y;
  } IntVec2, *IntVec2Ptr;

  typedef struct {
    s32 x, y;
  } S32Vec2, *S32Vec2Ptr;

  typedef struct {
    int x, y, z;
  } IntVec3, *IntVec3Ptr;

  typedef struct {
    s32 x, y, z;
  } S32Vec, S32Vec3, *S32VecPtr, *S32Vec3Ptr;

  typedef struct {
    f32 x, y, z, w;
  } Quaternion, Vec4, *QuaternionPtr, Qtrn, *QtrnPtr;

  typedef f32 Mtx[3][4];

  typedef f32 (*MtxPtr)[4];

  typedef f32 ROMtx[4][3];

  typedef f32 (*ROMtxPtr)[3];

  typedef f32 Mtx44[4][4];

  typedef f32 (*Mtx44Ptr)[4];

#ifdef __cplusplus
}
#endif

#endif // _DOLPHIN_GEOTYPES
