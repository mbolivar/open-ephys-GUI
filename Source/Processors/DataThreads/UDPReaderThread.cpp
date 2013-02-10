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

#ifndef _WIN32
// Make sure we get the XSI version of strerror_r() when using glibc.
#undef _GNU_SOURCE
#endif

#include "UDPReaderThread.h"

//---------------------------- GENERIC ROUTINES -------------------------------

/*
 * Try to socket() and bind() a new UDP socket at the specified port,
 * using IPv4 or IPv6 as available. On success, returns the socket. On
 * failure, returns a invalid socket.
 */
static sock_t getSocketOn(uint16_t port);
/* Portably test if a socket is valid. */
static bool isSocketValid(sock_t sock);
/* Portably close a socket. Returns true on success. */
static bool closeSocket(sock_t sock);
/*
 * Log information on the cause of a socket error ("log" currently
 * just means "print to std::cout").
 *
 * If err is NULL, error information is produced from a standard
 * place (e.g. errno, WSAGetLastError()). Otherwise, it's used
 * verbatim.
 */
static void logSocketError(const char* msg, const char* err=0);

// FIXME Just how big are the datagrams we can expect, anyway?
static const size_t sockBufLen = 2048;

// FIXME: this value is cargo-culted from FileReaderThread. It should
// be changed once we can agree on a network protocol, so the remote
// can tell us what it's sending.
static const int numChannels = 16;

UDPReaderThread::UDPReaderThread(SourceNode* sn, uint16_t _port)
    : DataThread(sn), sock(-1), port(_port), nLeftover(0)
{
    sock = getSocketOn(port);
    if (!isSocketValid(sock))
    {
        invalidate();
    }

    sockBuf = new int16_t[sockBufLen];
    thisSample = new float[numChannels];
    leftoverData = new int16_t[numChannels];
    const int readBacklog = 3;  // cargo-culted from FileReaderThread
    dataBuffer = new DataBuffer(numChannels, sockBufLen * readBacklog);
    dataBuffer->clear();
    eventCode = 0;
}

UDPReaderThread::~UDPReaderThread()
{
    // dataBuffer will be cleaned up by the ScopedPointer that holds it.
    delete leftoverData;
    delete thisSample;
    delete sockBuf;
    if (isSocketValid(sock))
        closeSocket(sock);
}

int UDPReaderThread::getNumChannels()
{
    return numChannels;
}

float UDPReaderThread::getSampleRate()
{
    // FIXME cargo-culted from FileReaderThread; read from remote instead.
    return 28000.0f;
}

float UDPReaderThread::getBitVolts()
{
    // FIXME cargo-culted from FileReaderThread; read from remote instead.
    return 0.0305f;
}

bool UDPReaderThread::foundInputSource()
{
    return isSocketValid(sock);
}

bool UDPReaderThread::startAcquisition()
{
    const ScopedLock lock(sockMutex);
    if (!foundInputSource())
        return false;
    startThread();
    return true;
}

bool UDPReaderThread::stopAcquisition()
{
    const ScopedLock lock(sockMutex);
    return closeSocket(sock);
}

//---------------------------- UNIX ROUTINES ----------------------------------

#ifndef _WIN32

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

static inline float* process(float* into, const int16_t* from, size_t n, float mul)
{
    float* p = into;
    for (size_t i = 0; i < n; i++)
    {
        // Negation is cargo-culted from FileReaderThread.
        *p++ = float(-from[i]) * mul;
    }
    return p;
}

// TODO: get timestamps from remote, not internal clock.
bool UDPReaderThread::updateBuffer()
{
    const ScopedLock lock(sockMutex);
    if (!foundInputSource())
        return false;

    const size_t sampleLen = (size_t)getNumChannels();

    // Read the new data.
    ssize_t nBytesRead = recvfrom(sock, sockBuf, sockBufLen * sizeof(*sockBuf),
                                  0, NULL, NULL);
    if (nBytesRead <= 0)
    {
        invalidate();
        return false;
    }
    // TODO: handle reads of partial samples.
    assert(nBytesRead % sizeof(*sockBuf) == 0);

    // If we read less than a full sample, everything is leftover.
    size_t nRead = nBytesRead / sizeof(*sockBuf);
    if (nRead + nLeftover < sampleLen)
    {
        memcpy(leftoverData + nLeftover, sockBuf, nBytesRead);
        nLeftover += nRead;
        return true;
    }

    // Fill first sample, including leftovers from last time.
    float* sp = thisSample;
    float bv = getBitVolts();
    uint64 ts = timer.getHighResolutionTicks();
    size_t nToFill = sampleLen - nLeftover;
    sp = process(sp, leftoverData, nLeftover, bv);
    sp = process(sp, sockBuf, nToFill, bv);
    dataBuffer->addToBuffer(thisSample, &ts, &eventCode, 1);

    // Main loop: fill thisSample until only leftovers are left
    nLeftover = (nRead - nToFill) % sampleLen;
    int16_t* bufp;
    for (bufp = sockBuf + nToFill;
         bufp < sockBuf + nRead - nLeftover;
         bufp += sampleLen)
    {
        process(thisSample, bufp, sampleLen, bv);
        ts = timer.getHighResolutionTicks();
        dataBuffer->addToBuffer(thisSample, &ts, &eventCode, 1);
    }

    // Copy any remaining leftovers for next time
    if (nLeftover)
        memcpy(leftoverData, bufp, nLeftover * sizeof(*leftoverData));

    return true;
}

void UDPReaderThread::invalidate()
{
    sock = -1;
}

static sock_t getSocketOn(uint16_t port)
{
    sock_t ret = -1;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    char portStr[20];
    snprintf(portStr, sizeof(portStr), "%u", port);

    struct addrinfo* aiResults;
    int gai_ret = getaddrinfo(NULL, portStr, &hints, &aiResults);
    if (gai_ret != 0)
    {
        // If getaddrinfo() fails, we might try falling back on
        // calling socket(AF_INET, SOCK_DGRAM, 0) ourselves to try to
        // get an IPv4 socket. But until that's actually a problem,
        // just scream and die.
        logSocketError("getaddrinfo",
                       gai_ret == EAI_SYSTEM ? NULL : gai_strerror(gai_ret));
        return ret;
    }

    for (struct addrinfo* arp = aiResults; arp != NULL; arp = arp->ai_next)
    {
        sock_t sckt = socket(arp->ai_family, arp->ai_socktype,
                             arp->ai_protocol);
        if (!isSocketValid(sckt))
        {
            continue;
        }
        if (bind(sckt, arp->ai_addr, arp->ai_addrlen) == -1)
        {
            closeSocket(sckt);
            continue;
        }

        // Success!
        ret = sckt;
        break;
    }

    freeaddrinfo(aiResults);
    return ret;
}

static bool isSocketValid(sock_t s)
{
    return s != -1;
}

static bool closeSocket(sock_t s)
{
    if (close(s) == -1)
    {
        logSocketError("close");
        return false;
    }
    return true;
}

static void logSocketError(const char* msg, const char* err)
{
    char errBuf[BUFSIZ];
    if (err != NULL)
    {
        strncpy(errBuf, err, sizeof(errBuf));
    }
    else
    {
        int sockErrnum = errno; // save errno in case strerror_r() clobbers it.
        if (strerror_r(sockErrnum, errBuf, sizeof(errBuf)) == -1)
        {
            switch (errno)
            {
                case ERANGE:
                    // `errBuf' is too small -- pretty much "can't happen",
                    // but do something reasonable.
                    snprintf(errBuf, sizeof(errBuf), "%d", sockErrnum);
                    break;
                case EINVAL:
                    // fall through
                default:
                    snprintf(errBuf, sizeof(errBuf), "unknown error %d", sockErrnum);
                    break;
            }
        }
    }
    std::cout << "Socket error: " << msg << ": " << errBuf << std::endl;
}

//---------------------------- WINDOWS ROUTINES -------------------------------

#else

bool UDPReaderThread::updateBuffer()
{
    // TODO (Windows). The UNIX version should mostly work, but there
    // are enough differences (e.g. getaddrinfo()'s return values are
    // different) to put this off.
    return false;
}

void UDPReaderThread::invalidate()
{
    sock = INVALID_SOCKET;
}

static sock_t getSocketOn(uint16_t port) // TODO (Windows)
{
    return INVALID_SOCKET;
}

static inline bool isSocketValid(sock_t s)
{
    return s != INVALID_SOCKET;
}

static bool closeSocket(sock_t s)
{
    if (closesocket(s) == SOCKET_ERROR)
    {
        logSocketError("closesocket");
        return false;
    }
    return true;
}

static void logSocketError(const char* msg, const char* err)
{
    /* TODO use WSAGetLastError() etc. */
    std::cout << "Socket error: " << msg << std::endl;
}

#endif
