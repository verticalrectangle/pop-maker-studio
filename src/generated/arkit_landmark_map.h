// ARKit semantic landmark -> vertex index mapping.
//
// The ARKit face mesh has 1220 vertices. The constants below map MediaPipe
// landmark indices to their ARKit mesh equivalents. Populated from:
//   - StackOverflow #53928038 (nose=9, leftEye=1064, rightEye=42, mouth=24/25)
//   - FaceLandmarks.com (eye/mouth contour groups)
//   - luca992's contour data (eye/mouth ring vertices)
//
// Indices marked // runtime are computed from the live mesh in
// face_filter_build_plan_from_arkit (their facial position varies with head
// pose and cannot be reliably hardcoded without the canonical mesh).
//
// Convention: L/R are the PERSON's left/right (ARKit +X = person's left).
#pragma once

static const int ARKIT_IRIS_L    = 1064;  // left iris center
static const int ARKIT_IRIS_R    = 42;    // right iris center
static const int ARKIT_NOSE_TIP  = 9;     // nose tip
static const int ARKIT_NOSE_L    = 0;     // left nose wing      // runtime
static const int ARKIT_NOSE_R    = 0;     // right nose wing     // runtime
static const int ARKIT_LIP_MID_L = 24;    // inner lip left mid (upper-lip center)
static const int ARKIT_LIP_MID_R = 25;    // inner lip right mid (lower-lip center)
static const int ARKIT_MOUTH_L   = 684;   // left mouth corner
static const int ARKIT_MOUTH_R   = 249;   // right mouth corner
static const int ARKIT_CHIN      = 0;     // chin tip            // runtime
static const int ARKIT_JAW_L     = 0;     // left jaw side       // runtime
static const int ARKIT_JAW_R     = 0;     // right jaw side      // runtime
static const int ARKIT_FOREHEAD  = 20;    // forehead top
static const int ARKIT_FACE_L    = 0;     // left face side      // runtime
static const int ARKIT_FACE_R    = 0;     // right face side     // runtime
static const int ARKIT_CHEEK_L   = 0;     // left cheek          // runtime
static const int ARKIT_CHEEK_R   = 0;     // right cheek         // runtime
static const int ARKIT_EYE_OUT_L = 1069;  // left outer eye corner
static const int ARKIT_EYE_OUT_R = 1090;  // right outer eye corner

// Left upper-lid chain (person's left), outer→inner.
// eyeTopLeft = [1069..1080] from FaceLandmarks.com; 7 evenly spaced.
static const int ARKIT_LID_L[7]  = {1069, 1071, 1073, 1075, 1077, 1079, 1080};
// Right upper-lid chain (person's right), outer→inner.
// eyeTopRight = [1090..1101] from FaceLandmarks.com; 7 evenly spaced.
static const int ARKIT_LID_R[7]  = {1090, 1092, 1094, 1096, 1098, 1100, 1101};

// Outer lip polygon (12 vertices), clockwise from person's left corner.
// Source: FaceLandmarks.com mouth contour groups.
//   684 (left corner) → 688, 691 (upper left) → 24 (top center) →
//   253, 250 (upper right) → 249 (right corner) → 248, 274 (lower right) →
//   25 (bottom center) → 710, 683 (lower left) → back to 684
static const int ARKIT_LIP_RING[12] = {684, 688, 691, 24, 253, 250,
                                        249, 248, 274, 25, 710, 683};

// Jaw contour chains (ear → chin), 5 vertices each.
// Runtime: computed from the live mesh.
static const int ARKIT_JAW_CHAIN_L[5] = {0, 0, 0, 0, 0};  // person's left  // runtime
static const int ARKIT_JAW_CHAIN_R[5] = {0, 0, 0, 0, 0};  // person's right // runtime
