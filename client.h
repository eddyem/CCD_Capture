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
#ifndef CLIENT_H__
#define CLIENT_H__

// waiting for answer timeout
#define ANSWER_TIMEOUT  1.0
// wait for exposition ends (between subsequent check calls)
#define WAIT_TIMEOUT    2.0
// client will disconnect after this time from last server message
#define CLIENT_TIMEOUT  10.0

// client-side functions
void client(int fd);

#endif // CLIENT_H__
