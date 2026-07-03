/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * Headless polygon triangulation (GLU tessellator, no GL context needed).
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#pragma once

#include <vector>

#include "S411Data.h"

/** Triangulate polygon rings (x = unwrapped lon, y = lat, even-odd rule)
 *  into a flat triangle list (3 positions per triangle). Runs on the CPU at
 *  load time so rendering never has to tessellate per frame. */
void S411Triangulate(const std::vector<std::vector<S411Pos>> &rings,
                     std::vector<S411Pos> &trisOut);
