/*
 * This file is part of the CCD_Capture project.
 * Copyright 2022 Edward V. Emelianov <edward.emelianoff@gmail.com>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once
#ifndef EVENTS_H__
#define EVENTS_H__

#include <stdlib.h>
#include <stdio.h>
#include <GL/glut.h>
#include <GL/glext.h>
#include <GL/freeglut.h>

extern float Z; // координата Z (zoom)

void keyPressed(unsigned char key, int x, int y);
//void keySpPressed(int key, int x, int y);
void mousePressed(int key, int state, int x, int y);
void mouseMove(int x, int y);
void createMenu();
void menuEvents(int opt);
//void mouseWheel(int button, int dir, int x, int y);

#endif // EVENTS_H__
