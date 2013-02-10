/*
    ------------------------------------------------------------------

    This file is part of the Open Ephys GUI
    Copyright (C) 2013 LeafLabs, LLC.

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __UDPREADERTHREAD_H_40D41976__
#define __UDPREADERTHREAD_H_40D41976__

#ifdef _WIN32
#include <Windows.h>
#include <Winsock2.h>
#endif
#include "../../../JuceLibraryCode/JuceHeader.h"
#include "DataThread.h"
#include <stdint.h>

class SourceNode;
#ifdef _WIN32
typedef SOCKET sock_t;
#else
typedef int sock_t;
#endif

/**

   @file UDPReaderThread.h

   @brief DataThread for reading data from a UDP socket on localhost.

   This is only available on UNIX-style platforms. You can construct
   an instance on Windows, but it won't do anything (e.g.,
   foundInputSource() will always return false).

   @see DataThread

 */
class UDPReaderThread : public DataThread
{
public:

    UDPReaderThread(SourceNode* sn, uint16_t port);
    ~UDPReaderThread();

    int getNumChannels();
    float getSampleRate();
    float getBitVolts();
    bool foundInputSource();
    bool startAcquisition();
    bool stopAcquisition();
    bool updateBuffer();

private:

    sock_t sock;               // Socket we read from
    const uint16_t port;       // Port we listen on
    int16_t* sockBuf;          // Intermediate buffer for recvfrom()
    CriticalSection sockMutex;

    // Array of length getNumChannels(). Data from sockBuf goes here
    // before getting passed on to dataBuffer.
    float* thisSample;
    // Data from last call to updateBuffer() that overflowed a
    // sample. They'll get shipped off next time.
    int16_t* leftoverData;
    // Number of leftover items from last time.
    size_t nLeftover;

    /*
     * Mark the instance invalid; called with sockMutex held.
     *
     * Afterwards, functions such as foundInputSource() and
     * updateBuffer() will fail.
     */
    void invalidate();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UDPReaderThread);
};

#endif  // __UDPREADERTHREAD_H_40D41976__
