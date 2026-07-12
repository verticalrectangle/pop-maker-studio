// ARKit semantic landmark -> vertex index mapping.
//
// The ARKit face mesh has 1220 vertices. The constants below are STUBS (set to
// 0) and MUST be populated on a Mac by inspecting ARFaceAnchor.geometry
// vertices or the canonical FaceMesh.obj in Xcode:
//   /Applications/Xcode.app/Contents/Developer/Library/ARKit/FaceMesh.obj
//
// Once populated, regenerate this header with tools/gen_arkit_mesh.py.
#pragma once

static const int ARKIT_IRIS_L    = 0;  // left iris center
static const int ARKIT_IRIS_R    = 0;  // right iris center
static const int ARKIT_NOSE_TIP  = 0;  // nose tip
static const int ARKIT_NOSE_L    = 0;  // left nose wing
static const int ARKIT_NOSE_R    = 0;  // right nose wing
static const int ARKIT_LIP_MID_L = 0;  // inner lip left mid
static const int ARKIT_LIP_MID_R = 0;  // inner lip right mid
static const int ARKIT_MOUTH_L   = 0;  // left mouth corner
static const int ARKIT_MOUTH_R   = 0;  // right mouth corner
static const int ARKIT_CHIN      = 0;  // chin tip
static const int ARKIT_JAW_L     = 0;  // left jaw side
static const int ARKIT_JAW_R     = 0;  // right jaw side
static const int ARKIT_FOREHEAD  = 0;  // forehead top
static const int ARKIT_FACE_L    = 0;  // left face side
static const int ARKIT_FACE_R    = 0;  // right face side
static const int ARKIT_CHEEK_L   = 0;  // left cheek
static const int ARKIT_CHEEK_R   = 0;  // right cheek
static const int ARKIT_EYE_OUT_L = 0;  // left outer eye corner
static const int ARKIT_EYE_OUT_R = 0;  // right outer eye corner

static const int ARKIT_LID_L[7]  = {0, 0, 0, 0, 0, 0, 0};  // left upper lid chain
static const int ARKIT_LID_R[7]  = {0, 0, 0, 0, 0, 0, 0};  // right upper lid chain
static const int ARKIT_LIP_RING[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // outer lip polygon
static const int ARKIT_JAW_CHAIN_L[5] = {0, 0, 0, 0, 0};  // left jaw contour (ear -> chin)
static const int ARKIT_JAW_CHAIN_R[5] = {0, 0, 0, 0, 0};  // right jaw contour (ear -> chin)
