"""Face framing rules shared by crop_media (server.py) and tools/album_portrait.py.

Album framing — the Remain in Light cover portrait: a square whose side is 1.8875x the
cheek-to-cheek width (landmarks 234..454), centred on the cheeks, with the nose tip
(landmark 1) at 56.24% of the height. Equivalent to "forehead-to-chin fills ~57% of the
square, chin at 80% height", but anchored on landmarks every face model places alike
(the hairline and chin points drift by 5-7% between MediaPipe and PMS's tracker).
"""

ALBUM_SIDE_PER_CHEEK_WIDTH = 1.8875
ALBUM_NOSE_Y = 0.5624


def album_box(landmarks_px, width: int, height: int) -> tuple[int, int, int]:
    """(x0, y0, side) in pixels for 478 [x, y] pixel landmarks on a width x height frame."""
    left, right, nose = landmarks_px[234], landmarks_px[454], landmarks_px[1]
    side = int(round(ALBUM_SIDE_PER_CHEEK_WIDTH * abs(right[0] - left[0])))
    side = max(1, min(side, width, height))
    cx = (left[0] + right[0]) / 2
    x0 = int(round(min(max(cx - side / 2, 0), width - side)))
    y0 = int(round(min(max(nose[1] - ALBUM_NOSE_Y * side, 0), height - side)))
    return x0, y0, side
