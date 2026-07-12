// ARKit semantic landmark -> vertex index mapping.
//
// The ARKit face mesh has 1220 vertices. The constants below map MediaPipe
// landmark indices to their ARKit mesh equivalents. Populated from:
//   - StackOverflow #53928038 (nose=9, leftEye=1064, rightEye=42, mouth=24/25)
//   - FaceLandmarks.com (eye/mouth contour groups)
//   - luca992's contour data (eye/mouth ring vertices)
//
// IMPORTANT: Both StackOverflow and FaceLandmarks.com use the VIEWER's
// perspective (looking at the face from outside). "Left" = screen left =
// the person's RIGHT side. The constants below are corrected to the
// PERSON's perspective (L = person's left, R = person's right).
//
// Indices marked // runtime are computed from the live mesh in
// face_filter_build_plan_from_arkit (their facial position varies with head
// pose and cannot be reliably hardcoded without the canonical mesh).
//
// Convention: L/R are the PERSON's left/right (ARKit +X = person's left).
#pragma once

static const int ARKIT_IRIS_L    = 42;    // person's left iris center
static const int ARKIT_IRIS_R    = 1064;  // person's right iris center
static const int ARKIT_NOSE_TIP  = 9;     // nose tip
static const int ARKIT_NOSE_L    = 0;     // person's left nose wing   // runtime
static const int ARKIT_NOSE_R    = 0;     // person's right nose wing  // runtime
static const int ARKIT_LIP_MID_L = 24;    // inner lip left mid (upper-lip center)
static const int ARKIT_LIP_MID_R = 25;    // inner lip right mid (lower-lip center)
static const int ARKIT_MOUTH_L   = 249;   // person's left mouth corner
static const int ARKIT_MOUTH_R   = 684;   // person's right mouth corner
static const int ARKIT_CHIN      = 0;     // chin tip            // runtime
static const int ARKIT_JAW_L     = 0;     // person's left jaw side   // runtime
static const int ARKIT_JAW_R     = 0;     // person's right jaw side  // runtime
static const int ARKIT_FOREHEAD  = 20;    // forehead top
static const int ARKIT_FACE_L    = 0;     // person's left face side  // runtime
static const int ARKIT_FACE_R    = 0;     // person's right face side // runtime
static const int ARKIT_CHEEK_L   = 0;     // person's left cheek      // runtime
static const int ARKIT_CHEEK_R   = 0;     // person's right cheek     // runtime
static const int ARKIT_EYE_OUT_L = 1101;  // person's left outer eye corner
static const int ARKIT_EYE_OUT_R = 1069;  // person's right outer eye corner

// Person's left upper-lid chain, outer→inner.
// FaceLandmarks.com eyeTopRight = [1090..1101] (viewer's right = person's left).
// Mesh topology BFS confirms 1101 is far from nose (outer), 1090 is near nose (inner).
static const int ARKIT_LID_L[7]  = {1101, 1100, 1098, 1096, 1094, 1092, 1090};
// Person's right upper-lid chain, outer→inner.
// FaceLandmarks.com eyeTopLeft = [1069..1080] (viewer's left = person's right).
static const int ARKIT_LID_R[7]  = {1069, 1071, 1073, 1075, 1077, 1079, 1080};

// Outer lip polygon (12 vertices), clockwise from person's left corner.
// Source: FaceLandmarks.com mouth contour groups (corrected to person's perspective).
//   249 (person's L corner) → 253, 250 (upper left) → 24 (top center) →
//   691, 688 (upper right) → 684 (person's R corner) → 683, 710 (lower right) →
//   25 (bottom center) → 274, 248 (lower left) → back to 249
static const int ARKIT_LIP_RING[12] = {249, 253, 250, 24, 691, 688,
                                        684, 683, 710, 25, 274, 248};

// Jaw contour chains (ear → chin), 5 vertices each.
// Runtime: computed from the live mesh.
static const int ARKIT_JAW_CHAIN_L[5] = {0, 0, 0, 0, 0};  // person's left  // runtime
static const int ARKIT_JAW_CHAIN_R[5] = {0, 0, 0, 0, 0};  // person's right // runtime
