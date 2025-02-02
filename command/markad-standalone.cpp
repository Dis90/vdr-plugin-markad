/*
 * markad-standalone.cpp: A program for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <ctype.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <pthread.h>
#include <poll.h>
#include <locale.h>
#include <libintl.h>
#include <execinfo.h>
#include <mntent.h>
#include <utime.h>
#include <math.h>
#include <limits.h>
#include <errno.h>
#include <dirent.h>

#include "markad-standalone.h"
#include "version.h"
#include "logo.h"
#include "index.h"

bool SYSLOG = false;
bool LOG2REC = false;
cDecoder *ptr_cDecoder = NULL;
cExtractLogo *ptr_cExtractLogo = NULL;
cMarkAdStandalone *cmasta = NULL;
bool restartLogoDetectionDone = false;
int SysLogLevel = 2;
bool abortNow = false;
struct timeval startAll, endAll = {};
struct timeval startPass1, startPass2, startPass3, startPass4, endPass1, endPass2, endPass3, endPass4 = {};
int logoSearchTime_ms = 0;
int logoChangeTime_ms = 0;
int decodeTime_us = 0;


static inline int ioprio_set(int which, int who, int ioprio) {
#if defined(__i386__)
#define __NR_ioprio_set         289
#elif defined(__ppc__)
#define __NR_ioprio_set         273
#elif defined(__x86_64__)
#define __NR_ioprio_set         251
#elif defined(__arm__)
#define __NR_ioprio_set         314
#elif defined(__ia64__)
#define __NR_ioprio_set        1274
#else
#define __NR_ioprio_set           0
#endif
    if (__NR_ioprio_set) {
        return syscall(__NR_ioprio_set, which, who, ioprio);
    }
    else {
        fprintf(stderr,"set io prio not supported on this system\n");
        return 0; // just do nothing
    }
}


static inline int ioprio_get(int which, int who) {
#if defined(__i386__)
#define __NR_ioprio_get         290
#elif defined(__ppc__)
#define __NR_ioprio_get         274
#elif defined(__x86_64__)
#define __NR_ioprio_get         252
#elif defined(__arm__)
#define __NR_ioprio_get         315
#elif defined(__ia64__)
#define __NR_ioprio_get        1275
#else
#define __NR_ioprio_get           0
#endif
    if (__NR_ioprio_get) {
        return syscall(__NR_ioprio_get, which, who);
    }
    else {
        fprintf(stderr,"get io prio not supported on this system\n");
        return 0; // just do nothing
    }

}


void syslog_with_tid(int priority, const char *format, ...) {
    va_list ap;
    if ((SYSLOG) && (!LOG2REC)) {
        priority = LOG_ERR;
        char fmt[255];
        snprintf(fmt, sizeof(fmt), "[%d] %s", getpid(), format);
        va_start(ap, format);
        vsyslog(priority, fmt, ap);
        va_end(ap);
    }
    else {
        char buf[27] = {0};
        const time_t now = time(NULL);
        if (ctime_r(&now, buf)) {
            buf[strlen(buf) - 6] = 0;
        }
        else dsyslog("ctime_r failed");
        char fmt[255];
        char prioText[10];
        switch (priority) {
            case LOG_ERR:   strcpy(prioText,"ERROR:"); break;
            case LOG_INFO : strcpy(prioText,"INFO: "); break;
            case LOG_DEBUG: strcpy(prioText,"DEBUG:"); break;
            case LOG_TRACE: strcpy(prioText,"TRACE:"); break;
            default:        strcpy(prioText,"?????:"); break;
        }
        snprintf(fmt, sizeof(fmt), "%s%s [%d] %s %s", LOG2REC ? "":"markad: ", buf, getpid(), prioText, format);
        va_start(ap, format);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
        fflush(stdout);
    }
}


cOSDMessage::cOSDMessage(const char *hostName, int portNumber) {
    msg=NULL;
    host = strdup(hostName);
    ALLOC(strlen(host)+1, "host");
    port = portNumber;
    SendMessage(this);
}


cOSDMessage::~cOSDMessage() {
    if (tid) pthread_join(tid, NULL);
    if (msg) {
        FREE(strlen(msg)+1, "msg");
        free(msg);
    }
    if (host) {
        FREE(strlen(host)+1, "host");
        free((void*) host);
    }
}


bool cOSDMessage::ReadReply(int fd, char **reply) {
    usleep(400000);
    char c = ' ';
    int repsize = 0;
    int msgsize = 0;
    if (reply) *reply = NULL;
    do {
        struct pollfd fds;
        fds.fd = fd;
        fds.events = POLLIN;
        fds.revents = 0;
        int ret = poll(&fds, 1, 600);

        if (ret <= 0) return false;
        if (fds.revents != POLLIN) return false;
        if (read(fd, &c, 1) < 0) return false;
        if ((reply) && (c != 10) && (c != 13)) {
            msgsize++;
            while ((msgsize + 5) > repsize) {
                repsize += 80;
                char *tmp = (char *) realloc(*reply, repsize);
                if (!tmp) {
                    free(*reply);
                    *reply = NULL;
                    return false;
                }
                else {
                    *reply = tmp;
                }
            }
            (*reply)[msgsize - 1] = c;
            (*reply)[msgsize] = 0;
        }
    }
    while (c != '\n');
    return true;
}


void *cOSDMessage::SendMessage(void *posd) {
    cOSDMessage *osd = static_cast<cOSDMessage *>(posd);

    struct hostent *host = gethostbyname(osd->host);
    if (!host) {
        osd->tid = 0;
        return NULL;
    }

    struct sockaddr_in name;
    name.sin_family = AF_INET;
    name.sin_port = htons(osd->port);
    memcpy(&name.sin_addr.s_addr, host->h_addr, host->h_length);
    uint size = sizeof(name);

    int sock;
    sock=socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    if (connect(sock, (struct sockaddr *)&name, size) != 0 ) {
        close(sock);
        return NULL;
    }

    char *reply = NULL;
    if (!osd->ReadReply(sock, &reply)) {
        if (reply) free(reply);
        close(sock);
        return NULL;
    }

    ssize_t ret;
    if (osd->msg) {
        if (reply) free(reply);
        ret=write(sock, "MESG ", 5);
        if (ret != (ssize_t) - 1) ret = write(sock,osd->msg,strlen(osd->msg));
        if (ret != (ssize_t) - 1) ret = write(sock, "\r\n", 2);

        if (!osd->ReadReply(sock) || (ret == (ssize_t) - 1)) {
            close(sock);
            return NULL;
        }
    }
    else {
        if (reply) {
            char *cs = strrchr(reply, ';');
            if (cs) {
                cs += 2;
                trcs(cs);
            }
            else {
                trcs("UTF-8"); // just a guess
            }
            free(reply);
        }
        else {
            trcs("UTF-8"); // just a guess
        }
    }
    ret=write(sock, "QUIT\r\n", 6);
    if (ret != (ssize_t) - 1) osd->ReadReply(sock);
    close(sock);
    return NULL;
}


int cOSDMessage::Send(const char *format, ...) {
    if (tid) pthread_join(tid, NULL);
    if (msg) free(msg);
    va_list ap;
    va_start(ap, format);
    if (vasprintf(&msg, format, ap) == -1) {
        va_end(ap);
        return -1;
    }
    ALLOC(strlen(msg)+1, "msg");
    va_end(ap);

    if (pthread_create(&tid, NULL, (void *(*) (void *))&SendMessage, (void *) this) != 0 ) return -1;
    return 0;
}


void cMarkAdStandalone::CalculateCheckPositions(int startframe) {
    dsyslog("cMarkAdStandalone::CalculateCheckPositions(): startframe %i (%dmin %2ds)", startframe, static_cast<int>(startframe / macontext.Video.Info.framesPerSecond / 60), static_cast<int>(startframe / macontext.Video.Info.framesPerSecond) % 60);

    if (!length) {
        dsyslog("CalculateCheckPositions(): length of recording not found, set to 100h");
        length = 100 * 60 * 60; // try anyway, set to 100h
        startframe = macontext.Video.Info.framesPerSecond * 2 * 60;  // assume default pretimer of 2min
    }
    if (!macontext.Video.Info.framesPerSecond) {
        esyslog("video frame rate of recording not found");
        return;
    }

    if (startframe < 0) {   // recodring start is too late
        isyslog("recording started too late, set start mark to start of recording");
        sMarkAdMark mark = {};
        mark.position = 1;  // do not use position 0 because this will later be deleted
        mark.type = MT_RECORDINGSTART;
        AddMark(&mark);
        startframe = macontext.Video.Info.framesPerSecond * 6 * 60;  // give 6 minutes to get best mark type for this recording
    }
    dsyslog("cMarkAdStandalone::CalculateCheckPositions(): use frame rate %i", static_cast<int>(macontext.Video.Info.framesPerSecond));

    iStart = -startframe;
    iStop = -(startframe + macontext.Video.Info.framesPerSecond * length) ;   // iStop change from - to + when frames reached iStop

    iStartA = abs(iStart);
    iStopA = startframe + macontext.Video.Info.framesPerSecond * (length + macontext.Config->astopoffs);
    chkSTART = iStartA + macontext.Video.Info.framesPerSecond * 4 * MAXRANGE; //  fit for later broadcast start
    chkSTOP = startframe + macontext.Video.Info.framesPerSecond * (length + macontext.Config->posttimer);

    dsyslog("cMarkAdStandalone::CalculateCheckPositions(): length of recording:   %4ds (%3dmin %2ds)", length, length / 60, length % 60);
    dsyslog("cMarkAdStandalone::CalculateCheckPositions(): assumed start frame:  %5d  (%3dmin %2ds)", iStartA, static_cast<int>(iStartA / macontext.Video.Info.framesPerSecond / 60), static_cast<int>(iStartA / macontext.Video.Info.framesPerSecond) % 60);
    dsyslog("cMarkAdStandalone::CalculateCheckPositions(): assumed stop frame:  %6d  (%3dmin %2ds)", iStopA, static_cast<int>(iStopA / macontext.Video.Info.framesPerSecond / 60), static_cast<int>(iStopA / macontext.Video.Info.framesPerSecond) % 60);
    dsyslog("cMarkAdStandalone::CalculateCheckPositions(): chkSTART set to:     %6d  (%3dmin %2ds)", chkSTART, static_cast<int>(chkSTART / macontext.Video.Info.framesPerSecond / 60), static_cast<int>(chkSTART / macontext.Video.Info.framesPerSecond) % 60);
    dsyslog("cMarkAdStandalone::CalculateCheckPositions(): chkSTOP set to:      %6d  (%3dmin %2ds)", chkSTOP, static_cast<int>(chkSTOP / macontext.Video.Info.framesPerSecond / 60), static_cast<int>(chkSTOP / macontext.Video.Info.framesPerSecond) % 60);
}


void cMarkAdStandalone::CheckStop() {
    LogSeparator(true);
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckStop(): start check stop (%i)", frameCurrent);

    char *indexToHMSF = marks.IndexToHMSF(iStopA, &macontext);
        if (indexToHMSF) {
            dsyslog("assumed stop position (%i) at %s", iStopA, indexToHMSF);
            FREE(strlen(indexToHMSF)+1, "indexToHMSF");
            free(indexToHMSF);
        }
    DebugMarks();     //  only for debugging

    // remove logo change marks
    RemoveLogoChangeMarks();

// try MT_CHANNELSTOP
    int delta = macontext.Video.Info.framesPerSecond * MAXRANGE;
    cMark *end = marks.GetAround(3 * delta, iStopA, MT_CHANNELSTOP);   // do not increase, we will get mark from last ad stop
    if (end) {
        dsyslog("cMarkAdStandalone::CheckStop(): MT_CHANNELSTOP found at frame %i", end->position);
        cMark *cStart = marks.GetPrev(end->position, MT_CHANNELSTART);      // if there is short befor a channel start, this stop mark belongs to next recording
        if (cStart) {
            if ((end->position - cStart->position) < delta) {
                dsyslog("cMarkAdStandalone::CheckStop(): MT_CHANNELSTART found short before at frame %i with delta %ds, MT_CHANNELSTOP is not valid, try to find stop mark short before", cStart->position, static_cast<int> ((end->position - cStart->position) / macontext.Video.Info.framesPerSecond));
                end = marks.GetAround(delta, iStopA - delta, MT_CHANNELSTOP);
                if (end) dsyslog("cMarkAdStandalone::CheckStop(): MT_CHANNELSTOP found short before at frame (%d)", end->position);
            }
            else {
                cMark *cStartFirst = marks.GetNext(0, MT_CHANNELSTART);  // get first channel start mark
                if (cStartFirst) {
                    int deltaC = (end->position - cStartFirst->position) / macontext.Video.Info.framesPerSecond;
                    if (deltaC < 305) {  // changed from 300 to 305
                    dsyslog("cMarkAdStandalone::CheckStop(): first channel start mark and possible channel end mark to near, this belongs to the next recording");
                    end = NULL;
                    }
                }
            }
        }
    }
    else dsyslog("cMarkAdStandalone::CheckStop(): no MT_CHANNELSTOP mark found");
    if (end) marks.DelWeakFromTo(marks.GetFirst()->position + 1, end->position, MT_CHANNELCHANGE); // delete all weak marks, except start mark

// try MT_ASPECTSTOP
    if (!end) {
        end = marks.GetAround(3 * delta, iStopA, MT_ASPECTSTOP);      // try MT_ASPECTSTOP
        if (end) {
            dsyslog("cMarkAdStandalone::CheckStop(): MT_ASPECTSTOP found at frame (%d)", end->position);
            if ((macontext.Info.AspectRatio.num == 4) && (macontext.Info.AspectRatio.den == 3)) {
                dsyslog("cMarkAdStandalone::CheckStop(): delete all weak marks");
                marks.DelWeakFromTo(marks.GetFirst()->position + 1, end->position, MT_ASPECTCHANGE); // delete all weak marks, except start mark
            }
            else { // 16:9 broadcast with 4:3 broadcast after, maybe ad between and we have a better logo stop mark
                cMark *logoStop = marks.GetPrev(end->position, MT_LOGOSTOP);
                if (logoStop) {
                    int diff = (end->position - logoStop->position) /  macontext.Video.Info.framesPerSecond;
                    dsyslog("cMarkAdStandalone::CheckStop(): found logo stop (%d) %ds before aspect ratio end mark (%d)", logoStop->position, diff, end->position);
                    if (diff <= 111) {  // changed from 100 to 111
                        dsyslog("cMarkAdStandalone::CheckStop(): advertising before, use logo stop mark");
                        end = logoStop;
                    }
                }
            }
        }
        else dsyslog("cMarkAdStandalone::CheckStop(): no MT_ASPECTSTOP mark found");
    }

// try MT_HBORDERSTOP
    if (!end) {
        end = marks.GetAround(5 * delta, iStopA, MT_HBORDERSTOP);         // increased from 3 to 5
        if (end) {
            dsyslog("cMarkAdStandalone::CheckStop(): MT_HBORDERSTOP found at frame %i", end->position);
            cMark *prevHStart = marks.GetPrev(end->position, MT_HBORDERSTART);
            if (prevHStart && (prevHStart->position > iStopA)) {
                dsyslog("cMarkAdStandalone::CheckStop(): previous hborder start mark (%d) is after assumed stop (%d), hborder stop mark (%d) is invalid", prevHStart->position, iStopA, end->position);
                // check if we got first hborder stop of next broadcast
                cMark *hBorderStopPrev = marks.GetPrev(end->position, MT_HBORDERSTOP);
                if (hBorderStopPrev) {
                    int diff = (iStopA - hBorderStopPrev->position) / macontext.Video.Info.framesPerSecond;
                    if (diff <= 476) { // maybe recording length is wrong
                        dsyslog("cMarkAdStandalone::CheckStop(): previous hborder stop mark (%d) is %ds before assumed stop, take this as stop mark", hBorderStopPrev->position, diff);
                        end = hBorderStopPrev;
                    }
                    else {
                        dsyslog("cMarkAdStandalone::CheckStop(): previous hborder stop mark (%d) is %ds before assumed stop, not valid", hBorderStopPrev->position, diff);
                        end = NULL;
                    }
                }
                else {
                    end = NULL;
                }
            }
        }
        else dsyslog("cMarkAdStandalone::CheckStop(): no MT_HBORDERSTOP mark found");
    }

// try MT_VBORDERSTOP
    if (!end) {
        end = marks.GetAround(3*delta, iStopA, MT_VBORDERSTOP);
        if (end) {
            dsyslog("cMarkAdStandalone::CheckStop(): MT_VBORDERSTOP found at frame %i", end->position);
            cMark *prevVStart = marks.GetPrev(end->position, MT_VBORDERSTART);
            if (prevVStart) {
                dsyslog("cMarkAdStandalone::CheckStop(): vertial border start and stop found, delete weak marks except start mark");
                marks.DelWeakFromTo(marks.GetFirst()->position + 1, INT_MAX, MT_VBORDERCHANGE);
            }
        }
        else dsyslog("cMarkAdStandalone::CheckStop(): no MT_VBORDERSTOP mark found");
    }

// try MT_LOGOSTOP
#define MAX_LOGO_END_MARK_FACTOR 2.7 // changed from 3 to 2.7 to prevent too early logo stop marks
    if (!end) {  // try any logo stop
        // delete possible logo end marks with very near logo start mark before
        bool isInvalid = true;
        while (isInvalid) {
            end = marks.GetAround(MAX_LOGO_END_MARK_FACTOR * delta, iStopA, MT_LOGOSTOP);
            if (end) {
                dsyslog("cMarkAdStandalone::CheckStop(): MT_LOGOSTOP found at frame %i", end->position);
                cMark *prevLogoStart = marks.GetPrev(end->position, MT_LOGOSTART);
                if (prevLogoStart) {
                    int deltaLogoStart = (end->position - prevLogoStart->position) / macontext.Video.Info.framesPerSecond;
                    if (deltaLogoStart < 9) { // do not increase because of SIXX and SAT.1 has very short logo change at the end of recording, which are sometimes not detected
                                              // sometimes we can not detect it at the end of the broadcast as info logo because text changes (noch eine Folge -> <Name der Folge>)
                        dsyslog("cMarkAdStandalone::CheckStop(): logo stop mark (%d) is invalid, logo start mark (%d) only %ds before", end->position, prevLogoStart->position, deltaLogoStart);
                        marks.Del(end);
                        marks.Del(prevLogoStart);
                    }
                    else {
                        dsyslog("cMarkAdStandalone::CheckStop(): logo stop mark (%d) is valid, logo start mark (%d) is %ds before", end->position, prevLogoStart->position, deltaLogoStart);
                        isInvalid = false;
                    }
                }
                else {
                    dsyslog("cMarkAdStandalone::CheckStop(): no logo start mark before found");
                    isInvalid = false;
                }
            }
            else {
                dsyslog("cMarkAdStandalone::CheckStop(): no logo stop mark found");
                isInvalid = false;
            }
        }
        // detect very short channel start before, in this case logo stop is invalid
        if (end) {
            cMark *prevChannelStart = marks.GetPrev(end->position, MT_CHANNELSTART);
            if (prevChannelStart) {
               int deltaChannelStart = (end->position - prevChannelStart->position) / macontext.Video.Info.framesPerSecond;
               if (deltaChannelStart <= 20) {
                   dsyslog("cMarkAdStandalone::CheckStop(): logo stop mark (%d) is invalid, channel start mark (%d) only %ds before", end->position, prevChannelStart->position, deltaChannelStart);
                   end = NULL;
                }
            }
        }

        // detect very short logo stop/start before assumed stop mark, they are text previews over the logo (e.g. SAT.1)
        if (end) {
            cMark *prevLogoStart = marks.GetPrev(end->position, MT_LOGOSTART);
            cMark *prevLogoStop  = marks.GetPrev(end->position, MT_LOGOSTOP);
            if (prevLogoStart && prevLogoStop) {
                int deltaLogoStart = (end->position - prevLogoStart->position) / macontext.Video.Info.framesPerSecond;
                int deltaLogoPrevStartStop = (prevLogoStart->position - prevLogoStop->position) / macontext.Video.Info.framesPerSecond;
#define CHECK_START_DISTANCE_MAX 13  // changed from 12 to 13
#define CHECK_START_STOP_LENGTH_MAX 4  // changed from 2 to 4
                if ((deltaLogoStart <= CHECK_START_DISTANCE_MAX) && (deltaLogoPrevStartStop <= CHECK_START_STOP_LENGTH_MAX)) {
                    dsyslog("cMarkAdStandalone::CheckStop(): logo start (%d) stop (%d) pair before assumed end mark too near %ds (expect >%ds) and too short %ds (expect >%ds), this is a text preview over the logo, delete marks", prevLogoStart->position, prevLogoStop->position, deltaLogoStart, CHECK_START_DISTANCE_MAX, deltaLogoPrevStartStop, CHECK_START_STOP_LENGTH_MAX);
                    marks.Del(prevLogoStart);
                    marks.Del(prevLogoStop);
                }
                else dsyslog("cMarkAdStandalone::CheckStop(): logo start (%d) stop (%d) pair before assumed end mark is far %ds (expect >%ds) or long %ds (expect >%ds), end mark is valid", prevLogoStart->position, prevLogoStop->position, deltaLogoStart, CHECK_START_DISTANCE_MAX, deltaLogoPrevStartStop, CHECK_START_STOP_LENGTH_MAX);
            }
            prevLogoStop = marks.GetPrev(end->position, MT_LOGOSTOP); // maybe different if deleted above
            if (prevLogoStop) {
                int deltaLogo = (end->position - prevLogoStop->position) / macontext.Video.Info.framesPerSecond;
#define CHECK_STOP_BEFORE_MIN 14 // if stop before is too near, maybe recording length is too big
                if (deltaLogo < CHECK_STOP_BEFORE_MIN) {
                    dsyslog("cMarkAdStandalone::CheckStop(): logo stop before too near %ds (expect >=%ds), use (%d) as stop mark", deltaLogo, CHECK_STOP_BEFORE_MIN, prevLogoStop->position);
                    end = prevLogoStop;
                }
                else dsyslog("cMarkAdStandalone::CheckStop(): logo stop before at (%d) too far away %ds (expect <%ds), no alternative", prevLogoStop->position, deltaLogo, CHECK_STOP_BEFORE_MIN);
            }
        }

        // check if very eary logo end mark is end of preview
        if (end) {
            int beforeAssumed = (iStopA - end->position) / macontext.Video.Info.framesPerSecond;
            dsyslog("cMarkAdStandalone::CheckStop(): end mark (%d) %ds before assumed stop (%d)", end->position, beforeAssumed, iStopA);
            if (beforeAssumed >= 218) {
                cMark *prevLogoStart = marks.GetPrev(end->position, MT_LOGOSTART);
                // ad before
                cMark *prevLogoStop = NULL;
                if (prevLogoStart) prevLogoStop = marks.GetPrev(prevLogoStart->position, MT_LOGOSTOP);
                // broadcast after
                cMark *nextLogoStart = marks.GetNext(end->position, MT_LOGOSTART);
                cMark *nextLogoStop = NULL;
                if (nextLogoStart) nextLogoStop = marks.GetNext(end->position, MT_LOGOSTOP);

                if (prevLogoStart && prevLogoStop && nextLogoStart && !nextLogoStop) {
                    int adBefore = (prevLogoStart->position - prevLogoStop->position) / macontext.Video.Info.framesPerSecond;
                    dsyslog("cMarkAdStandalone::CheckStop(): advertising before from (%d) to (%d) %3ds", prevLogoStart->position, prevLogoStop->position, adBefore);
                    int adAfter = (nextLogoStart->position - end->position) / macontext.Video.Info.framesPerSecond;
                    dsyslog("cMarkAdStandalone::CheckStop(): advertising after  from (%d) to (%d) %3ds", end->position, nextLogoStart->position, adAfter);
                    int broadcastBefore = (end->position - prevLogoStart->position) / macontext.Video.Info.framesPerSecond;
                    dsyslog("cMarkAdStandalone::CheckStop(): broadcast   before from (%d) to (%d) %3ds", prevLogoStart->position, end->position, broadcastBefore);
                    if (adAfter <= 33) {
                        dsyslog("cMarkAdStandalone::CheckStop(): advertising after logo end mark too short, mark not valid");
                        end = NULL;
                    }
                }
            }
        }
        else dsyslog("cMarkAdStandalone::CheckStop(): no MT_LOGOSTOP mark found");
    }

    if (!end) {
        end = marks.GetAround(1.1 * delta, iStopA + delta, MT_STOP, 0x0F);    // try any type of stop mark, start search after iStopA and accept only  short before
        if (end) dsyslog("cMarkAdStandalone::CheckStop(): weak end mark found at frame %i", end->position);
        else dsyslog("cMarkAdStandalone::CheckStop(): no end mark found");
    }

    cMark *lastStart = marks.GetAround(INT_MAX, frameCurrent, MT_START, 0x0F);
    if (end) {
        dsyslog("cMarkAdStandalone::CheckStop(): found end mark at (%i)", end->position);
        cMark *mark = marks.GetFirst();
        while (mark) {
            if ((mark->position >= iStopA-macontext.Video.Info.framesPerSecond*MAXRANGE) && (mark->position < end->position) && ((mark->type & 0xF0) < (end->type & 0xF0))) { // delete all weak marks
                dsyslog("cMarkAdStandalone::CheckStop(): found stronger end mark delete mark (%i)", mark->position);
                cMark *tmp = mark;
                mark = mark->Next();
                marks.Del(tmp);
                continue;
            }
            mark = mark->Next();
        }

        if ((end->type == MT_NOBLACKSTOP) && (end->position < iStopA)) {        // if stop mark is MT_NOBLACKSTOP and it is not after iStopA try next, better save than sorry
           cMark *end2 = marks.GetAround(delta, end->position + 2*delta, MT_STOP, 0x0F);
           if (end2) {
               dsyslog("cMarkAdStandalone::CheckStop(): stop mark is week, use next stop mark at (%i)", end2->position);
               end = end2;
           }
        }

        indexToHMSF = marks.IndexToHMSF(end->position, &macontext);
        if (indexToHMSF) {
            isyslog("using mark on position (%i) type 0x%X at %s as stop mark", end->position,  end->type, indexToHMSF);
            FREE(strlen(indexToHMSF)+1, "indexToHMSF");
            free(indexToHMSF);
        }

        dsyslog("cMarkAdStandalone::CheckStop(): delete all marks after final stop mark at (%d)", end->position);
        marks.DelTill(end->position, false);

        if ( end->position < iStopA - 5 * delta ) {    // last found stop mark too early, adding STOP mark at the end, increased from 3 to 5
                                                     // this can happen by audio channel change too if the next broadcast has also 6 channels
            if ( ( lastStart) && ( lastStart->position > end->position ) ) {
                isyslog("last STOP mark results in to short recording, set STOP at the end of the recording (%i)", iFrameCurrent);
                sMarkAdMark markNew = {};
                markNew.position = iFrameCurrent;
                markNew.type = MT_ASSUMEDSTOP;
                AddMark(&markNew);
            }
        }
    }
    else {  // no valid stop mark found
        // try if there is any late MT_ASPECTSTOP
        cMark *aFirstStart = marks.GetNext(0, MT_ASPECTSTART);
        if (aFirstStart) {
            cMark *aLastStop = marks.GetPrev(INT_MAX, MT_ASPECTSTOP);
            if (aLastStop && (aLastStop->position > iStopA)) {
                dsyslog("cMarkAdStandalone::CheckStop(): start mark is MT_ASPECTSTART (%d) found very late MT_ASPECTSTOP at (%d)", aFirstStart->position, aLastStop->position);
                end = aLastStop;
                marks.DelTill(end->position, false);
            }
        }
        if (!end) {
            dsyslog("cMarkAdStandalone::CheckStop(): no stop mark found, add stop mark at the last frame (%i)", iFrameCurrent);
            sMarkAdMark mark = {};
            mark.position = iFrameCurrent;  // we are lost, add a end mark at the last iframe
            mark.type = MT_ASSUMEDSTOP;
            AddMark(&mark);
        }
    }

    // delete all black sceen marks expect start or end mark
    dsyslog("cMarkAdStandalone::CheckStop(): move all black screen marks except start and end mark to black screen list");
    cMark *mark = marks.GetFirst();
    while (mark) {
        if (mark != marks.GetFirst()) {
            if (mark == marks.GetLast()) break;
            if ((mark->type & 0xF0) == MT_BLACKCHANGE) {
                cMark *tmp = mark;
                mark = mark->Next();
                marks.Del(tmp);
                continue;
            }
        }
        mark = mark->Next();
    }

    iStop = iStopA = 0;
    gotendmark = true;

    DebugMarks();     //  only for debugging
    dsyslog("cMarkAdStandalone::CheckStop(): end check stop");
    LogSeparator();

}


// check if last stop mark is start of closing credits without logo or hborder
// move stop mark to end of closing credit
// <stopMark> last logo or hborder stop mark
// return: true if closing credits was found and last logo stop mark position was changed
//
bool cMarkAdStandalone::MoveLastStopAfterClosingCredits(cMark *stopMark) {
    if (!stopMark) return false;
    dsyslog("cMarkAdStandalone::MoveLastStopAfterClosingCredits(): check closing credits without logo after position (%d)", stopMark->position);

    cDetectLogoStopStart *ptr_cDetectLogoStopStart = new cDetectLogoStopStart(&macontext, ptr_cDecoder, recordingIndexMark, NULL);
    ALLOC(sizeof(*ptr_cDetectLogoStopStart), "ptr_cDetectLogoStopStart");

    int endPos = stopMark->position + (25 * macontext.Video.Info.framesPerSecond);  // try till 15s after stopMarkPosition
    int newPosition = -1;
    if (ptr_cDetectLogoStopStart->Detect(stopMark->position, endPos, false)) {
        newPosition = ptr_cDetectLogoStopStart->ClosingCredit();
    }

    FREE(sizeof(*ptr_cDetectLogoStopStart), "ptr_cDetectLogoStopStart");
    delete ptr_cDetectLogoStopStart;

    if (newPosition > stopMark->position) {
        dsyslog("cMarkAdStandalone::MoveLastStopAfterClosingCredits(): closing credits found, move logo stop mark to position (%d)", newPosition);
        marks.Move(&macontext, stopMark, newPosition, "closing credits");
        return true;
    }
    else {
        dsyslog("cMarkAdStandalone::MoveLastStopAfterClosingCredits(): no closing credits found");
        return false;
    }
}


// remove stop/start logo mark pair if it detecs a part in the broadcast with logo changes
// some channel e.g. TELE5 plays with the logo in the broadcast
//
void cMarkAdStandalone::RemoveLogoChangeMarks() {  // for performance reason only known and tested channels
    struct timeval startTime, stopTime;
    gettimeofday(&startTime, NULL);

    LogSeparator(true);
    dsyslog("cMarkAdStandalone::RemoveLogoChangeMarks(): start detect and remove logo stop/start mark pairs with special logo");

    if (evaluateLogoStopStartPair) {  // we need a new clean instance of the object
        FREE(sizeof(*evaluateLogoStopStartPair), "evaluateLogoStopStartPair");
        delete evaluateLogoStopStartPair;
    }
    evaluateLogoStopStartPair = new cEvaluateLogoStopStartPair(&macontext, &marks, &blackMarks, iStart, chkSTART, iStopA);
    ALLOC(sizeof(*evaluateLogoStopStartPair), "evaluateLogoStopStartPair");

    char *indexToHMSFStop = NULL;
    char *indexToHMSFStart = NULL;
    int stopPosition = 0;
    int startPosition = 0;
    int isLogoChange = 0;
    int isInfoLogo = 0;

    // alloc new objects
    ptr_cDecoderLogoChange = new cDecoder(macontext.Config->threads, recordingIndexMark);
    ALLOC(sizeof(*ptr_cDecoderLogoChange), "ptr_cDecoderLogoChange");
    ptr_cDecoderLogoChange->DecodeDir(directory);

    cExtractLogo *ptr_cExtractLogoChange = new cExtractLogo(&macontext, macontext.Video.Info.AspectRatio, recordingIndexMark);
    ALLOC(sizeof(*ptr_cExtractLogoChange), "ptr_cExtractLogoChange");

    cDetectLogoStopStart *ptr_cDetectLogoStopStart = new cDetectLogoStopStart(&macontext, ptr_cDecoderLogoChange, recordingIndexMark, evaluateLogoStopStartPair);
    ALLOC(sizeof(*ptr_cDetectLogoStopStart), "ptr_cDetectLogoStopStart");

    // loop through all logo stop/start pairs
    while (evaluateLogoStopStartPair->GetNextPair(&stopPosition, &startPosition, &isLogoChange, &isInfoLogo)) {
        LogSeparator();
        // free from loop before
        if (indexToHMSFStop) {
            FREE(strlen(indexToHMSFStop)+1, "indexToHMSF");
            free(indexToHMSFStop);
        }
        if (indexToHMSFStart) {
            FREE(strlen(indexToHMSFStart)+1, "indexToHMSF");
            free(indexToHMSFStart);
        }
        // get time of marks and log marks
        indexToHMSFStop = marks.IndexToHMSF(stopPosition, &macontext);
        indexToHMSFStart = marks.IndexToHMSF(startPosition, &macontext);
        if (indexToHMSFStop && indexToHMSFStart) {
            dsyslog("cMarkAdStandalone::RemoveLogoChangeMarks(): check logo stop (%d) at %s and logo start (%d) at %s, isInfoLogo %d", stopPosition, indexToHMSFStop, startPosition, indexToHMSFStart, isInfoLogo);
        }
        if (ptr_cDetectLogoStopStart->Detect(stopPosition, startPosition, false)) {
            // check info logo before logo mark position
            if ((isInfoLogo >= 0) && ptr_cDetectLogoStopStart->IsInfoLogo()) {
                // found info logo part
                if (indexToHMSFStop && indexToHMSFStart) {
                    dsyslog("cMarkAdStandalone::RemoveLogoChangeMarks(): info logo found between frame (%i) at %s and (%i) at %s, deleting marks between this positions", stopPosition, indexToHMSFStop, startPosition, indexToHMSFStart);
                }
                evaluateLogoStopStartPair->SetIsInfoLogo(stopPosition, startPosition);
                marks.DelFromTo(stopPosition, startPosition, MT_LOGOCHANGE);  // maybe there a false start/stop inbetween
            }
            if ((isLogoChange >= 0) && ptr_cDetectLogoStopStart->IsLogoChange()) {
                if (indexToHMSFStop && indexToHMSFStart) {
                    isyslog("logo has changed between frame (%i) at %s and (%i) at %s, deleting marks between this positions", stopPosition, indexToHMSFStop, startPosition, indexToHMSFStart);
                }
                marks.DelFromTo(stopPosition, startPosition, MT_LOGOCHANGE);  // maybe there a false start/stop inbetween
            }
        }
    }

    // delete last timer string
    if (indexToHMSFStop) {
        FREE(strlen(indexToHMSFStop)+1, "indexToHMSF");
        free(indexToHMSFStop);
    }
    if (indexToHMSFStart) {
        FREE(strlen(indexToHMSFStart)+1, "indexToHMSF");
        free(indexToHMSFStart);
    }

    // free objects
    FREE(sizeof(*ptr_cExtractLogoChange), "ptr_cExtractLogoChange");
    delete ptr_cExtractLogoChange;
    FREE(sizeof(*ptr_cDecoderLogoChange), "ptr_cDecoderLogoChange");
    delete ptr_cDecoderLogoChange;
    FREE(sizeof(*ptr_cDetectLogoStopStart), "ptr_cDetectLogoStopStart");
    delete ptr_cDetectLogoStopStart;

    dsyslog("cMarkAdStandalone::RemoveLogoChangeMarks(): marks after detect and remove logo stop/start mark pairs with special logo");
    DebugMarks();     //  only for debugging

    gettimeofday(&stopTime, NULL);
    time_t sec = stopTime.tv_sec - startTime.tv_sec;
    suseconds_t usec = stopTime.tv_usec - startTime.tv_usec;
    if (usec < 0) {
        usec += 1000000;
        sec--;
    }
    logoChangeTime_ms += sec * 1000 + usec / 1000;

    dsyslog("cMarkAdStandalone::RemoveLogoChangeMarks(): end detect and remove logo stop/start mark pairs with special logo");
    LogSeparator();
}


void cMarkAdStandalone::CheckStart() {
    LogSeparator(true);
    dsyslog("cMarkAdStandalone::CheckStart(): checking start at frame (%d) check start planed at (%d)", frameCurrent, chkSTART);
    dsyslog("cMarkAdStandalone::CheckStart(): assumed start frame %i", iStartA);
    DebugMarks();     //  only for debugging
#define IGNORE_AT_START 10   // ignore this number of frames at the start for marks, they are initial marks from recording before

    int hBorderStopPosition = 0;
    int delta = macontext.Video.Info.framesPerSecond * MAXRANGE;

    cMark *begin = marks.GetAround(delta, 1, MT_RECORDINGSTART);  // do we have an incomplete recording ?
    if (begin) {
        dsyslog("cMarkAdStandalone::CheckStart(): found MT_RECORDINGSTART (%i), use this as start mark for the incomplete recording", begin->position);
        // delete short stop marks without start mark
        cMark *stopMark = marks.GetNext(0, MT_CHANNELSTOP);
        if (stopMark) {
            int diff = stopMark->position / macontext.Video.Info.framesPerSecond;
            if ((diff < 30) && (marks.Count(MT_CHANNELSTART) == 0)) {
                dsyslog("cMarkAdStandalone::CheckStart(): delete stop mark (%d) without start mark", stopMark->position);
                marks.Del(stopMark->position);
            }
        }
    }

// try to find a audio channel mark
    for (short int stream = 0; stream < MAXSTREAMS; stream++) {
        if ((macontext.Info.Channels[stream]) && (macontext.Audio.Info.Channels[stream]) && (macontext.Info.Channels[stream] != macontext.Audio.Info.Channels[stream])) {
            char as[20];
            switch (macontext.Info.Channels[stream]) {
                case 1:
                    strcpy(as, "mono");
                    break;
                case 2:
                    strcpy(as, "stereo");
                    break;
                case 6:
                    strcpy(as, "dd5.1");
                    break;
                default:
                    strcpy(as, "??");
                    break;
            }
            char ad[20];
            switch (macontext.Audio.Info.Channels[stream]) {
                case 1:
                    strcpy(ad, "mono");
                    break;
                case 2:
                    strcpy(ad, "stereo");
                    break;
                case 6:
                    strcpy(ad, "dd5.1");
                    break;
                default:
                    strcpy(ad, "??");
                break;
            }
            isyslog("audio description in info (%s) wrong, we have %s", as, ad);
        }
        macontext.Info.Channels[stream] = macontext.Audio.Info.Channels[stream];

        if (macontext.Config->decodeAudio && macontext.Info.Channels[stream]) {
            if ((macontext.Info.Channels[stream] == 6) && (macontext.Audio.Options.ignoreDolbyDetection == false)) {
                isyslog("DolbyDigital5.1 audio whith 6 Channels in stream %i detected", stream);
                if (macontext.Audio.Info.channelChange) {
                    dsyslog("cMarkAdStandalone::CheckStart(): channel change detected, disable logo/border/aspect detection");
                    bDecodeVideo = false;
                    macontext.Video.Options.ignoreAspectRatio = true;
                    macontext.Video.Options.ignoreLogoDetection = true;
                    macontext.Video.Options.ignoreBlackScreenDetection = true;
                    marks.Del(MT_ASPECTSTART);
                    marks.Del(MT_ASPECTSTOP);

                    // start mark must be around iStartA
                    begin = marks.GetAround(delta * 3, iStartA, MT_CHANNELSTART);  // decrease from 4
                    if (!begin) {          // previous recording had also 6 channels, try other marks
                        dsyslog("cMarkAdStandalone::CheckStart(): no audio channel start mark found");
                    }
                    else {
                        dsyslog("cMarkAdStandalone::CheckStart(): audio channel start mark found at %d", begin->position);
                        if (begin->position > iStopA) {  // this could be a very short recording, 6 channel is in post recording
                            dsyslog("cMarkAdStandalone::CheckStart(): audio channel start mark after assumed stop mark not valid");
                            begin = NULL;
                        }
                        else {
                            if (marks.GetNext(begin->position, MT_HBORDERSTART) || marks.GetNext(begin->position, MT_VBORDERSTART)) macontext.Video.Info.hasBorder = true;
                            marks.Del(MT_LOGOSTART);   // we do not need the weaker marks if we found a MT_CHANNELSTART
                            marks.Del(MT_LOGOSTOP);
                            marks.Del(MT_HBORDERSTART);
                            marks.Del(MT_HBORDERSTOP);
                            marks.Del(MT_VBORDERSTART);
                            marks.Del(MT_VBORDERSTOP);
                        }
                    }
                }
                else dsyslog("cMarkAdStandalone::CheckStart(): no audio channel change found till now, do not disable logo/border/aspect detection");
            }
            else {
                if (macontext.Audio.Options.ignoreDolbyDetection == true) isyslog("disabling AC3 decoding (from logo)");
                if ((macontext.Info.Channels[stream]) && (macontext.Audio.Options.ignoreDolbyDetection == false))
                    isyslog("broadcast with %i audio channels of stream %i",macontext.Info.Channels[stream], stream);
                if (inBroadCast) {  // if we have channel marks but we are now with 2 channels inBroascast, delete these
                    macontext.Video.Options.ignoreAspectRatio = false;   // then we have to find other marks
                    macontext.Video.Options.ignoreLogoDetection = false;
                    macontext.Video.Options.ignoreBlackScreenDetection = false;
                }
            }
        }
    }
    if (begin && inBroadCast) { // set recording aspect ratio for logo search at the end of the recording
        macontext.Info.AspectRatio.num = macontext.Video.Info.AspectRatio.num;
        macontext.Info.AspectRatio.den = macontext.Video.Info.AspectRatio.den;
        macontext.Info.checkedAspectRatio = true;
        isyslog("Video with aspectratio of %i:%i detected", macontext.Info.AspectRatio.num, macontext.Info.AspectRatio.den);
    }
    if (!begin && inBroadCast) {
        cMark *chStart = marks.GetNext(0, MT_CHANNELSTART);
        cMark *chStop = marks.GetPrev(INT_MAX, MT_CHANNELSTOP);
        if (chStart && chStop && (chStart->position > chStop->position)) {
            dsyslog("cMarkAdStandalone::CheckStart(): channel start after channel stop found, delete all weak marks between");
            marks.DelWeakFromTo(chStop->position, chStart->position, MT_CHANNELCHANGE);
        }
    }
    if (!begin && !inBroadCast) {
        dsyslog("cMarkAdStandalone::CheckStart(): we are not in broadcast at frame (%d), trying to find channel start mark anyway", frameCurrent);
        begin = marks.GetAround(delta*4, iStartA, MT_CHANNELSTART);
        // check if the start mark is from previous recording
        if (begin) {
            cMark *lastChannelStop = marks.GetPrev(INT_MAX, MT_CHANNELSTOP);
            if (lastChannelStop && (lastChannelStop->position <= chkSTART)) {
                dsyslog("cMarkAdStandalone::CheckStart(): last channel stop mark at frame (%d) is too early, ignore channel marks are from previous recording", lastChannelStop->position);
                begin = NULL;
            }
        }
    }
    if (begin) marks.DelWeakFromTo(0, INT_MAX, MT_CHANNELCHANGE); // we have a channel start mark, delete all weak marks
    else {                                                        // no channel start mark found, cleanup invalid channel stop marks
        cMark *cStart = marks.GetNext(0, MT_CHANNELSTART);
        cMark *cStop  = marks.GetNext(0, MT_CHANNELSTOP);
        if (!cStart && cStop) {  // channel stop mark and no channel start mark
            int pos = cStop->position;
            char *comment = NULL;
            dsyslog("cMarkAdStandalone::CheckStart(): channel stop without start mark found (%i), assume as start mark of the following recording, delete it", pos);
            marks.Del(pos);
            if (asprintf(&comment,"assumed start from channel stop (%d)", pos) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            begin=marks.Add(MT_ASSUMEDSTART, pos, comment);
            FREE(strlen(comment)+1, "comment");
            free(comment);
        }
    }

// try to find a aspect ratio mark
    if (!begin) {
        if ((macontext.Info.AspectRatio.num == 0) || (macontext.Video.Info.AspectRatio.den == 0)) {
            isyslog("no video aspect ratio found in vdr info file");
            macontext.Info.AspectRatio.num = macontext.Video.Info.AspectRatio.num;
            macontext.Info.AspectRatio.den = macontext.Video.Info.AspectRatio.den;
        }
        // check marks and correct if necessary
        cMark *aStart = marks.GetAround(chkSTART, chkSTART + IGNORE_AT_START, MT_ASPECTSTART);   // check if we have ascpect ratio START/STOP in start part
        cMark *aStopAfter = NULL;
        cMark *aStopBefore = NULL;
        if (aStart) {
            dsyslog("cMarkAdStandalone::CheckStart(): found aspect ratio start mark at (%d), video info is %d:%d", aStart->position, macontext.Info.AspectRatio.num, macontext.Info.AspectRatio.den);
            aStopAfter = marks.GetNext(aStart->position, MT_ASPECTSTOP);
            aStopBefore = marks.GetPrev(aStart->position, MT_ASPECTSTOP);
        }
        bool earlyAspectChange = false;
        if (aStart && aStopAfter) {  // we are in the first ad, do not correct aspect ratio from info file
            dsyslog("cMarkAdStandalone::CheckStart(): found very early aspect ratio change at (%i) and (%i)", aStart->position, aStopAfter->position);
            earlyAspectChange = true;
        }

        // check aspect ratio info from vdr info file
        cMark *firstStop = marks.GetNext(0, MT_ASPECTSTOP);
        if (!aStart && firstStop && (firstStop->position > (iStopA * 0.8))) {   // we have no start mark and a stop mark at the end, this is next recording
            dsyslog("cMarkAdStandalone::CheckStart(): first aspectio ratio stop (%d) near assumed end mark, we are in next broadcast", firstStop->position);
        }
        else { // we have marks to work with
            bool wrongAspectInfo = false;
            if ((macontext.Info.AspectRatio.num == 16) && (macontext.Info.AspectRatio.den == 9)) {  // vdr info file tells 16:9 video
                if (aStart && aStopBefore && (aStopBefore->position > 0)) { // found 16:9 -> 4:3 -> 16:9, this can not be a 16:9 video
                    dsyslog("cMarkAdStandalone::CheckStart(): found aspect ratio change 16:9 to 4:3 at (%d) to 16:9 at (%d), video info is 16:9, this must be wrong",
                                                                                                                                             aStopBefore->position, aStart->position);
                    wrongAspectInfo = true;
                }
                if (!wrongAspectInfo && (macontext.Video.Info.AspectRatio.num == 4) && (macontext.Video.Info.AspectRatio.den == 3) && inBroadCast) {
                    dsyslog("cMarkAdStandalone::CheckStart(): vdr info tells 16:9 but we are in broadcast and found 4:3, vdr info file must be wrong");
                    wrongAspectInfo = true;
                }
                if (aStart && !wrongAspectInfo) {
                    cMark *logoStopBefore = marks.GetPrev(aStart->position, MT_LOGOSTOP);
                    if (logoStopBefore) {
                        int diff = (aStart->position - logoStopBefore->position) / macontext.Video.Info.framesPerSecond;
                        if (diff <= 4) {
                            dsyslog("cMarkAdStandalone::CheckStart(): vdr info tells 16:9 but we found logo stop mark (%d) short before aspect ratio start mark (%d)",
                                                                                                                                          logoStopBefore->position, aStart->position);
                            if (aStopBefore && aStopBefore->position == 0) { // this is 4:3 from previous recording, no valid mark
                                dsyslog("cMarkAdStandalone::CheckStart(): delete invalid aspect stop mark at (%d)", aStopBefore->position);
                                marks.Del(aStopBefore->position);
                            }
                            wrongAspectInfo = true;
                        }
                    }
                }
            }

            // fix wrong aspect ratio from vdr info file
            if (wrongAspectInfo || ((!earlyAspectChange) && ((macontext.Info.AspectRatio.num != macontext.Video.Info.AspectRatio.num) ||
                                                             (macontext.Info.AspectRatio.den != macontext.Video.Info.AspectRatio.den)))) {
                sAspectRatio newMarkAdAspectRatio;
                newMarkAdAspectRatio.num = 16;
                newMarkAdAspectRatio.den = 9;
                if ((macontext.Info.AspectRatio.num == 16) && (macontext.Info.AspectRatio.den == 9)) {
                    newMarkAdAspectRatio.num = 4;
                    newMarkAdAspectRatio.den = 3;
                }
                isyslog("video aspect description in info (%d:%d) wrong, correct to (%d:%d)", macontext.Info.AspectRatio.num, macontext.Info.AspectRatio.den, newMarkAdAspectRatio.num, newMarkAdAspectRatio.den);
                macontext.Info.AspectRatio.num = newMarkAdAspectRatio.num;
                macontext.Info.AspectRatio.den = newMarkAdAspectRatio.den;
                // we have to invert MT_ASPECTSTART and MT_ASPECTSTOP and fix position
                cMark *aMark = marks.GetFirst();
                while (aMark) {
                    if (aMark->type == MT_ASPECTSTART) {
                        aMark->type = MT_ASPECTSTOP;
                        aMark->position = recordingIndexMark->GetIFrameBefore(aMark->position - 1);
                    }
                    else {
                        if (aMark->type == MT_ASPECTSTOP) {
                            aMark->type = MT_ASPECTSTART;
                            aMark->position = recordingIndexMark->GetIFrameAfter(aMark->position + 1);
                        }
                    }
                    aMark = aMark->Next();
                }
            }
        }
        if (macontext.Info.vPidType == MARKAD_PIDTYPE_VIDEO_H264) {
            isyslog("HD video with aspectratio of %i:%i detected", macontext.Info.AspectRatio.num, macontext.Info.AspectRatio.den);
        }
        else {
            isyslog("SD video with aspectratio of %i:%i detected", macontext.Info.AspectRatio.num, macontext.Info.AspectRatio.den);
            if (((macontext.Info.AspectRatio.num == 4) && (macontext.Info.AspectRatio.den == 3))) {
                isyslog("logo/border detection disabled");
                bDecodeVideo = false;
                macontext.Video.Options.ignoreAspectRatio = false;
                macontext.Video.Options.ignoreLogoDetection = true;
                macontext.Video.Options.ignoreBlackScreenDetection = true;
                marks.Del(MT_CHANNELSTART);
                marks.Del(MT_CHANNELSTOP);
                // start mark must be around iStartA
                begin = marks.GetAround(delta * 4, iStartA, MT_ASPECTSTART);
                if (begin) {
                    dsyslog("cMarkAdStandalone::CheckStart(): aspect ratio start mark found at (%i)", begin->position);
                    if (begin->position > abs(iStartA) / 4) {    // this is a valid start
                        dsyslog("cMarkAdStandalone::CheckStart(): aspect ratio start mark at (%i) is valid, delete all logo and border marks", begin->position);
                        marks.Del(MT_LOGOSTART);  // we found MT_ASPECTSTART, we do not need weeker marks
                        marks.Del(MT_LOGOSTOP);
                        marks.Del(MT_HBORDERSTART);
                        marks.Del(MT_HBORDERSTOP);
                        marks.Del(MT_VBORDERSTART);
                        marks.Del(MT_VBORDERSTOP);
                        macontext.Video.Options.ignoreHborder = true;
                        macontext.Video.Options.ignoreVborder = true;
                   }
                   else {
                       cMark *aStopNext = marks.GetNext(begin->position, MT_ASPECTSTOP);
                       if (aStopNext) dsyslog("cMarkAdStandalone::CheckStart(): found MT_ASPECTSTOP (%i)", aStopNext->position);
                       else {
                           dsyslog("cMarkAdStandalone::CheckStart(): MT_ASPECTSTART is not valid (%i), ignoring", begin->position);
                           marks.Del(begin->position);  // delete invalid start mark to prevent to be selected again later
                           begin = NULL;
                       }
                       if (begin && (begin->position <= IGNORE_AT_START)) {
                           dsyslog("cMarkAdStandalone::CheckStart(): only got start aspect ratio, ignoring");
                           begin = NULL;
                       }
                   }
                }
                else dsyslog("cMarkAdStandalone::CheckStart(): no MT_ASPECTSTART found");   // previous is 4:3 too, search another start mark
            }
            else { // recording is 16:9 but maybe we can get a MT_ASPECTSTART mark if previous recording was 4:3
                begin = marks.GetAround(delta * 3, iStartA, MT_ASPECTSTART);
                if (begin) {
                    dsyslog("cMarkAdStandalone::CheckStart(): MT_ASPECTSTART found at (%i) because previous recording was 4:3", begin->position);
                    cMark *begin3 = marks.GetAround(delta, begin->position, MT_VBORDERSTART);  // do not use this mark if there is a later vborder start mark
                    if (begin3 && (begin3->position >  begin->position)) {
                        dsyslog("cMarkAdStandalone::CheckStart(): found later MT_VBORDERSTAT, do not use MT_ASPECTSTART");
                        begin = NULL;
                    }
                    else {
                        begin3 = marks.GetAround(delta * 4, begin->position, MT_LOGOSTART);  // do not use this mark if there is a later logo start mark
                        if (begin3 && (begin3->position > begin->position)) {
                            dsyslog("cMarkAdStandalone::CheckStart(): found later MT_LOGOSTART, do not use MT_ASPECTSTART");
                            begin = NULL;
                        }
                    }
                }
            }
        }
        macontext.Info.checkedAspectRatio = true;
        if (begin && (macontext.Info.AspectRatio.num == 4) && (macontext.Info.AspectRatio.den == 3)) marks.DelWeakFromTo(0, INT_MAX, MT_ASPECTCHANGE); // delete all weak marks
    }

// try to find a horizontal border mark (MT_HBORDERSTART)
    if (!begin) {
        cMark *hStart = marks.GetAround(iStartA + delta, iStartA + delta, MT_HBORDERSTART);
        if (hStart) { // we found a hborder start mark
            dsyslog("cMarkAdStandalone::CheckStart(): horizontal border start found at (%i)", hStart->position);
            cMark *hStop = marks.GetNext(hStart->position, MT_HBORDERSTOP);  // if there is a MT_HBORDERSTOP short after the MT_HBORDERSTART, MT_HBORDERSTART is not valid
            if ( hStop && ((hStop->position - hStart->position) < (2 * delta))) {
                dsyslog("cMarkAdStandalone::CheckStart(): horizontal border stop (%i) short after horizontal border start (%i) found, this is end of broadcast before or a preview", hStop->position, hStart->position); // do not delete weak marks here because it can only be from preview
                hBorderStopPosition = hStop->position;  // maybe we can use this position as start mark if we found nothing else
                dsyslog("cMarkAdStandalone::CheckStart(): delete horizontal border stop (%d) mark", hStop->position);
                marks.Del(hStop->position); // delete hborder stop mark because we ignore hborder start mark
            }
            else {
                if (hStart->position >= IGNORE_AT_START) {  // position < IGNORE_AT_START is a hborder start from previous recording
                    dsyslog("cMarkAdStandalone::CheckStart(): delete VBORDER marks if any");
                    marks.Del(MT_VBORDERSTART);
                    marks.Del(MT_VBORDERSTOP);
                    begin = hStart;   // found valid horizontal border start mark
                    macontext.Video.Options.ignoreVborder = true;
                }
                else {
                    dsyslog("cMarkAdStandalone::CheckStart(): delete too early horizontal border mark (%d)", hStart->position);
                    marks.Del(hStart->position);
                    if (marks.Count(MT_HBORDERCHANGE, 0xF0) == 0) {
                        dsyslog("cMarkAdStandalone::CheckStart(): horizontal border since start, logo marks can not be valid");
                        marks.DelType(MT_LOGOCHANGE, 0xF0);
                    }
                }
            }
        }
        else { // we found no hborder start mark
            dsyslog("cMarkAdStandalone::CheckStart(): no horizontal border at start found, ignore horizontal border detection");
            macontext.Video.Options.ignoreHborder = true;
            cMark *hStop = marks.GetAround(iStartA + delta, iStartA + delta, MT_HBORDERSTOP);
            if (hStop) {
                int pos = hStop->position;
                char *comment = NULL;
                dsyslog("cMarkAdStandalone::CheckStart(): horizontal border stop without start mark found (%i), assume as start mark of the following recording", pos);
                marks.Del(pos);
                if (asprintf(&comment,"assumed start from horizontal border stop (%d)", pos) == -1) comment = NULL;
                ALLOC(strlen(comment)+1, "comment");
                begin=marks.Add(MT_ASSUMEDSTART, pos, comment);
                FREE(strlen(comment)+1, "comment");
                free(comment);
            }
        }
    }

// try to find a vertical border mark
    if (!begin) {
        cMark *vStart = marks.GetAround(iStartA + delta, iStartA + delta, MT_VBORDERSTART);  // do not find initial vborder start from previous recording
        if (!vStart) {
            dsyslog("cMarkAdStandalone::CheckStart(): no vertical border at start found, ignore vertical border detection");
            macontext.Video.Options.ignoreVborder = true;
            cMark *vStop = marks.GetAround(iStartA + delta, iStartA + delta, MT_VBORDERSTOP);
            if (vStop) {
                int pos = vStop->position;
                char *comment = NULL;
                dsyslog("cMarkAdStandalone::CheckStart(): vertical border stop without start mark found (%i), possible start mark of the following recording", pos);
                marks.Del(pos);
                if (asprintf(&comment,"assumed start from vertical border stop (%d)", pos) == -1) comment = NULL;
                ALLOC(strlen(comment)+1, "comment");
                marks.Add(MT_ASSUMEDSTART, pos, comment);
                FREE(strlen(comment)+1, "comment");
                free(comment);
            }
        }
        else {
            dsyslog("cMarkAdStandalone::CheckStart(): vertical border start found at (%i)", vStart->position);
            cMark *vStop = marks.GetNext(vStart->position, MT_VBORDERSTOP);  // if there is a MT_VBORDERSTOP short after the MT_VBORDERSTART, MT_VBORDERSTART is not valid
            if (vStop) {
                cMark *vNextStart = marks.GetNext(vStop->position, MT_VBORDERSTART);
                int markDiff = static_cast<int> (vStop->position - vStart->position) / macontext.Video.Info.framesPerSecond;
                dsyslog("cMarkAdStandalone::CheckStart(): vertical border stop found at (%d), %ds after vertical border start", vStop->position, markDiff);
                if (vNextStart) {
                    dsyslog("cMarkAdStandalone::CheckStart(): vertical border start (%d) after vertical border stop (%d) found, start mark at (%d) is valid", vNextStart->position, vStop->position, vStart->position);
                }
                else { // we have only start/stop vborder in start part, this can be the closing credits of recording before
                    dsyslog("cMarkAdStandalone::CheckStart(): no vertical border start found after start (%d) and stop (%d)", vStart->position, vStop->position);
                    // 228s opening credits with vborder -> invalid TODO
                    //  96s opening credits with vborder -> invalid
                    // 151s advertising in start area    -> valid
                    if ((markDiff <= 122) ||         // too short for a broadcast part
                        (frameCurrent > iStopA)) { // we got not in broadcast at chkSTART with a vborder mark
                        isyslog("vertical border stop at (%d) %ds after vertical border start (%i) in start part found, this is not valid, delete marks", vStop->position, markDiff, vStart->position);
                        marks.Del(vStop);
                        marks.Del(vStart);
                        vStart = NULL;
                    }
                }
            }
            if (vStart) {
                if (vStart->position >= IGNORE_AT_START) {  // early position is a vborder previous recording
                    dsyslog("cMarkAdStandalone::CheckStart(): delete HBORDER marks if any");
                    marks.Del(MT_HBORDERSTART);
                    marks.Del(MT_HBORDERSTOP);
                    begin = vStart;   // found valid vertical border start mark
                    macontext.Video.Info.hasBorder = true;
                    macontext.Video.Options.ignoreHborder = true;
                }
                else dsyslog("cMarkAdStandalone::CheckStart(): ignore vertical border start found at (%d)", vStart->position);
            }
        }
    }

// try to find a logo mark
    if (!begin) {
        RemoveLogoChangeMarks();
        cMark *lStart = marks.GetAround(iStartA + (2 * delta), iStartA, MT_LOGOSTART);   // increase from 1
        if (!lStart) {
            dsyslog("cMarkAdStandalone::CheckStart(): no logo start mark found");
        }
        else {
            char *indexToHMSF = marks.IndexToHMSF(lStart->position, &macontext);
            if (indexToHMSF) {
                dsyslog("cMarkAdStandalone::CheckStart(): logo start mark found on position (%i) at %s", lStart->position, indexToHMSF);
                FREE(strlen(indexToHMSF)+1, "indexToHMSF");
                free(indexToHMSF);
            }
            if (lStart->position  < (iStart / 8)) {  // start mark is too early, try to find a later mark
                cMark *lNextStart = marks.GetNext(lStart->position, MT_LOGOSTART);
                if (lNextStart && (lNextStart->position  > (iStart / 8)) && ((lNextStart->position - lStart->position) < (5 * delta))) {  // found later logo start mark
                    char *indexToHMSFStart = marks.IndexToHMSF(lNextStart->position, &macontext);
                    if (indexToHMSFStart) {
                        dsyslog("cMarkAdStandalone::CheckStart(): later logo start mark found on position (%i) at %s", lNextStart->position, indexToHMSFStart);
                        FREE(strlen(indexToHMSFStart)+1, "indexToHMSF");
                        free(indexToHMSFStart);
                    }
                    lStart = lNextStart;   // found better logo start mark
                }
            }
            bool isInvalid = true;
            while (isInvalid) {
                // if the logo start mark belongs to closing credits logo stop/start pair, treat it as valid
                if (evaluateLogoStopStartPair && evaluateLogoStopStartPair->GetIsClosingCredits(lStart->position)) break;

                // check next logo stop/start pair
                cMark *lStop = marks.GetNext(lStart->position, MT_LOGOSTOP);  // get next logo stop mark
                if (lStop) {  // there is a next stop mark in the start range
                    int distanceStartStop = (lStop->position - lStart->position) / macontext.Video.Info.framesPerSecond;
                    if (distanceStartStop < 55) {  // very short logo part, lStart is possible wrong, do not increase, first ad can be early
                                                   // changed from 144 to 23 to prevent "Teletext Untertitel Tafel ..." make start mark wrong
                                                   // change from 23 to 55, "Teletext Untertitel Tafel ..." should be now detected and ignored
                        indexToHMSF = marks.IndexToHMSF(lStop->position, &macontext);
                        if (indexToHMSF) {
                            dsyslog("cMarkAdStandalone::CheckStart(): logo stop mark found very short after start mark on position (%i) at %s, distance %ds", lStop->position, indexToHMSF, distanceStartStop);
                            FREE(strlen(indexToHMSF)+1, "indexToHMSF");
                            free(indexToHMSF);
                        }
                        cMark *lNextStart = marks.GetNext(lStop->position, MT_LOGOSTART); // get next logo start mark
                        if (lNextStart) {  // now we have logo start/stop/start, this can be a preview before broadcast start
                            indexToHMSF = marks.IndexToHMSF(lNextStart->position, &macontext);
                            int distanceStopNextStart = (lNextStart->position - lStop->position) / macontext.Video.Info.framesPerSecond;
                            if ((distanceStopNextStart <= 76) || // found start mark short after start/stop, use this as start mark, changed from 21 to 68 to 76
                                (distanceStartStop <= 10)) { // very short logo start stop is not valid
                                if (indexToHMSF) dsyslog("cMarkAdStandalone::CheckStart(): found start mark short after logo start/stop marks on position (%i) at %s", lNextStart->position, indexToHMSF);
                                lStart = lNextStart;
                            }
                            else {
                                isInvalid = false;
                                if (indexToHMSF) dsyslog("cMarkAdStandalone::CheckStart(): next logo start mark (%i) at %s too far away %d", lNextStart->position, indexToHMSF, distanceStopNextStart);
                            }
                            if (indexToHMSF) {
                                FREE(strlen(indexToHMSF)+1, "indexToHMSF");
                                free(indexToHMSF);
                            }
                        }
                        else isInvalid = false;
                    }
                    else {  // there is a next stop mark but too far away
                        dsyslog("cMarkAdStandalone::CheckStart(): next logo stop mark (%d) but too far away %ds", lStop->position, distanceStartStop);
                        isInvalid = false;
                    }
                }
                else { // the is no next stop mark
                    isInvalid = false;
                }
            }
            if (lStart->position  >= (iStart / 8)) {
                begin = lStart;   // found valid logo start mark
                marks.Del(MT_HBORDERSTART);  // there could be hborder from an advertising in the recording
                marks.Del(MT_HBORDERSTOP);
            }
            else dsyslog("cMarkAdStandalone::CheckStart(): logo start mark (%d) too early, ignoring", lStart->position);
        }
        if (begin) {
            dsyslog("cMarkAdStandalone::CheckStart(): disable border detection");  // avoid false detection of border
            macontext.Video.Options.ignoreHborder = true;
            macontext.Video.Options.ignoreVborder = true;
        }
    }

    if (begin && (begin->type != MT_RECORDINGSTART) && (begin->position <= IGNORE_AT_START)) {  // first frames are from previous recording
        dsyslog("cMarkAdStandalone::CheckStart(): start mark (%d) type 0x%X dropped because it is too early", begin->position, begin->type);
        begin = NULL;
    }

    if (!begin) {    // try anything
        marks.DelTill(1, &blackMarks);    // we do not want to have a start mark at position 0
        begin = marks.GetAround(iStartA + 3 * delta, iStartA, MT_START, 0x0F);  // increased from 2 to 3
        if (begin) {
            dsyslog("cMarkAdStandalone::CheckStart(): found start mark (%d) type 0x%X after search for any type", begin->position, begin->type);
            if (begin->type == MT_NOBLACKSTART) {
                int diff = 0;
                cMark *blackStop = marks.GetPrev(begin->position, MT_NOBLACKSTOP);
                if (blackStop) {
                    diff = 1000 * (begin->position - blackStop->position) / macontext.Video.Info.framesPerSecond; // trust long blackscreen
                    dsyslog("cMarkAdStandalone::CheckStart(): found found blackscreen from (%d) to (%d), length %dms", blackStop->position, begin->position, diff);
                }
                if ((diff < 800) && (begin->position > (iStartA + 2 * delta))) {
                    dsyslog("cMarkAdStandalone::CheckStart(): found only very late and short black screen start mark (%i), ignoring", begin->position);
                    begin = NULL;
                }
            }
            else {
                if ((begin->inBroadCast) || macontext.Video.Options.ignoreLogoDetection){  // test on inBroadCast because we have to take care of black screen marks in an ad
                    char *indexToHMSF = marks.IndexToHMSF(begin->position, &macontext);
                    if (indexToHMSF) {
                        dsyslog("cMarkAdStandalone::CheckStart(): found start mark (%i) type 0x%X at %s inBroadCast %i", begin->position, begin->type, indexToHMSF, begin->inBroadCast);
                        FREE(strlen(indexToHMSF)+1, "indexToHMSF");
                        free(indexToHMSF);
                    }
                }
                else { // mark in ad
                    char *indexToHMSF = marks.IndexToHMSF(begin->position, &macontext);
                    if (indexToHMSF) {
                        dsyslog("cMarkAdStandalone::CheckStart(): start mark found but not inBroadCast (%i) type 0x%X at %s inBroadCast %i, ignoring", begin->position, begin->type, indexToHMSF, begin->inBroadCast);
                        FREE(strlen(indexToHMSF)+1, "indexToHMSF");
                        free(indexToHMSF);
                    }
                    begin = NULL;
                }
            }
        }
    }
    if (begin && ((begin->position  / macontext.Video.Info.framesPerSecond) < 1) && (begin->type != MT_RECORDINGSTART)) { // do not accept marks in the first second, the are from previous recording, expect manual set MT_RECORDINGSTART fpr missed recording start
        dsyslog("cMarkAdStandalone::CheckStart(): start mark (%d) type 0x%X dropped because it is too early", begin->position, begin->type);
        begin = NULL;
    }

    // if we found a blackscreen start mark, check if we are in very long closing credit from previous recording
    if (begin && (begin->type == MT_NOBLACKSTART)) {
        bool isInvalid = true;
        while (isInvalid) {
            cMark *nextStop  = blackMarks.GetNext(begin->position, MT_NOBLACKSTOP);
            cMark *nextStart = blackMarks.GetNext(begin->position, MT_NOBLACKSTART);
            if (nextStart && nextStop) {
                int diff = (nextStop->position - begin->position) / macontext.Video.Info.framesPerSecond;
                int adLength = (nextStart->position - nextStop->position) / macontext.Video.Info.framesPerSecond;;
                dsyslog("cMarkAdStandalone::CheckStart(): next blackscreen from (%d) to (%d) in %ds, length %ds", nextStop->position, nextStart->position, diff, adLength);
                if ((diff <= 67) && (adLength >= 5)) {  // changed from 3 to 5, avoid long dark scenes
                    dsyslog("cMarkAdStandalone::CheckStart(): very long blackscreen short after, we are in the closing credits of previous recording");
                    begin = nextStart;
                }
                else isInvalid = false;
            }
            else isInvalid = false;
        }
    }

    if (begin) {
        marks.DelTill(begin->position, &blackMarks);    // delete all marks till start mark
        CalculateCheckPositions(begin->position);
        char *indexToHMSF = marks.IndexToHMSF(begin->position, &macontext);
        if (indexToHMSF) {
            isyslog("using mark on position (%i) type 0x%X at %s as start mark", begin->position, begin->type, indexToHMSF);
            FREE(strlen(indexToHMSF)+1, "indexToHMSF");
            free(indexToHMSF);
        }


        if ((begin->type == MT_VBORDERSTART) || (begin->type == MT_HBORDERSTART)) {
            isyslog("found %s borders, logo detection disabled",(begin->type == MT_HBORDERSTART) ? "horizontal" : "vertical");
            macontext.Video.Options.ignoreLogoDetection = true;
            macontext.Video.Options.ignoreBlackScreenDetection = true;
            marks.Del(MT_LOGOSTART);
            marks.Del(MT_LOGOSTOP);
        }

        dsyslog("cMarkAdStandalone::CheckStart(): delete all black screen marks except start mark");
        cMark *mark = marks.GetFirst();   // delete all black screen marks because they are weak, execpt the start mark
        while (mark) {
            if (((mark->type & 0xF0) == MT_BLACKCHANGE) && (mark->position > begin->position) ) {
                cMark *tmp = mark;
                mark = mark->Next();
                marks.Del(tmp);  // delete mark from normal list
                continue;
            }
            mark = mark->Next();
        }
    }
    else { //fallback
        // try hborder stop mark as start mark
        if (hBorderStopPosition > 0) {
            dsyslog("cMarkAdStandalone::CheckStart(): no valid start mark found, use MT_HBORDERSTOP from previous recoring as start mark");
            marks.Add(MT_ASSUMEDSTART, hBorderStopPosition, "start mark from border stop of previous recording*", true);
            marks.DelTill(hBorderStopPosition, &blackMarks);
        }
        else {  // set start after pre timer
            dsyslog("cMarkAdStandalone::CheckStart(): no valid start mark found, assume start time at pre recording time");
            marks.DelTill(iStart, &blackMarks);
            marks.Del(MT_NOBLACKSTART);  // delete all black screen marks
            marks.Del(MT_NOBLACKSTOP);
            sMarkAdMark mark = {};
            mark.position = iStart;
            mark.type = MT_ASSUMEDSTART;
            AddMark(&mark);
            CalculateCheckPositions(iStart);
        }
    }

    // now we have the final start mark, do fine tuning
    if (begin && (begin->type == MT_HBORDERSTART)) { // we found a valid hborder start mark, check black screen because of closing credits from broadcast before
        cMark *blackMark = blackMarks.GetNext(begin->position, MT_NOBLACKSTART);
        if (blackMark) {
            int diff =(blackMark->position - begin->position) / macontext.Video.Info.framesPerSecond;
            dsyslog("cMarkAdStandalone::CheckStart(): black screen (%d) after, distance %ds", blackMark->position, diff);
            if (diff <= 6) {
                dsyslog("cMarkAdStandalone::CheckStart(): move horizontal border (%d) to end of black screen (%d)", begin->position, blackMark->position);
                marks.Move(&macontext, begin, blackMark->position, "black screen");
            }
        }
   }

// count logo STOP/START pairs
    int countStopStart = 0;
    cMark *mark = marks.GetFirst();
    while (mark) {
        if ((mark->type == MT_LOGOSTOP) && mark->Next() && (mark->Next()->type == MT_LOGOSTART)) {
            countStopStart++;
        }
        mark = mark->Next();
    }
    if ((countStopStart >= 3) && begin) {
        isyslog("%d logo STOP/START pairs found after start mark, something is wrong with your logo", countStopStart);
        if (video->ReducePlanes()) {
            dsyslog("cMarkAdStandalone::CheckStart(): reduce logo processing to first plane and delete all marks after start mark (%d)", begin->position);
            marks.DelFrom(begin->position);
        }
    }

    iStart = 0;
    marks.Save(directory, &macontext, false);
    DebugMarks();     //  only for debugging
    LogSeparator();
    return;
}


void cMarkAdStandalone::LogSeparator(const bool main) {
    if (main) dsyslog("=======================================================================================================================");
    else      dsyslog("-----------------------------------------------------------------------------------------------------------------------");
}


// write all current marks to log file
//
void cMarkAdStandalone::DebugMarks() {           // write all marks to log file
    dsyslog("*************************************************************");
    dsyslog("cMarkAdStandalone::DebugMarks(): current marks:");
    cMark *mark = marks.GetFirst();
    while (mark) {
        char *indexToHMSF = marks.IndexToHMSF(mark->position, &macontext);
        if (indexToHMSF) {
            dsyslog("mark at position %6i type 0x%X at %s inBroadCast %i", mark->position, mark->type, indexToHMSF, mark->inBroadCast);
            FREE(strlen(indexToHMSF)+1, "indexToHMSF");
            free(indexToHMSF);
        }
        mark=mark->Next();
    }
    tsyslog("cMarkAdStandalone::DebugMarks(): current black screen marks:");
    mark = blackMarks.GetFirst();
    while (mark) {
        char *indexToHMSF = marks.IndexToHMSF(mark->position, &macontext);
        if (indexToHMSF) {
            tsyslog("mark at position %6i type 0x%X at %s inBroadCast %i", mark->position, mark->type, indexToHMSF, mark->inBroadCast);
            FREE(strlen(indexToHMSF)+1, "indexToHMSF");
            free(indexToHMSF);
        }
        mark=mark->Next();
    }
    dsyslog("*************************************************************");
}


void cMarkAdStandalone::CheckMarks() {           // cleanup marks that make no sense
    LogSeparator(true);
    cMark *mark = NULL;

    // remove invalid marks
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckMarks(): remove invalid marks");
    DebugMarks();     //  only for debugging
    mark = marks.GetFirst();
    while (mark) {
        // if first mark is a stop mark, remove it
        if (((mark->type & 0x0F) == MT_STOP) && (mark == marks.GetFirst())){
            dsyslog("Start with STOP mark, delete first mark");
            cMark *tmp = mark;
            mark = mark->Next();
            marks.Del(tmp);
            continue;
        }
        // start followed by start or stop followed by stop
        if ((((mark->type & 0x0F) == MT_STOP)  && (mark->Next()) && ((mark->Next()->type & 0x0F) == MT_STOP)) || // two stop or start marks, keep most used type, delete other
            (((mark->type & 0x0F) == MT_START) && (mark->Next()) && ((mark->Next()->type & 0x0F) == MT_START))) {
            if ((mark == marks.GetFirst()) && mark->Next()->position >  (macontext.Video.Info.framesPerSecond * (length + macontext.Config->astopoffs))) {
                dsyslog("cMarkAdStandalone::CheckMarks(): double start mark as first marks, second start mark near end, this is start mark of the next recording, delete this");
                marks.Del(mark->Next());
            }
            else {
                int count1 = marks.Count(mark->type);
                int count2 = marks.Count(mark->Next()->type);
                dsyslog("cMarkAdStandalone::CheckMarks(): mark (%i) type count %d, followed by same mark (%i) type count %d", mark->position, count1, mark->Next()->position, count2);
                if (count1 == count2) { // if equal, keep stronger type
                    if (mark->type > mark->Next()->type) count1++;
                    else                                 count2++;
                }
                if (count1 < count2) {
                    dsyslog("cMarkAdStandalone::CheckMarks(): delete mark (%d)", mark->position);
                    cMark *tmp = mark;
                    mark = mark->Next();
                    marks.Del(tmp);
                    continue;
                }
                else {
                    dsyslog("cMarkAdStandalone::CheckMarks(): delete stop mark (%d)", mark->Next()->position);
                   marks.Del(mark->Next());
                }
            }
        }

        // if stop/start distance is too big, remove pair
        if (((mark->type & 0x0F) == MT_STOP) && (mark->Next()) && ((mark->Next()->type & 0x0F) == MT_START)) {
            int diff = (mark->Next()->position - mark->position) / macontext.Video.Info.framesPerSecond;
            dsyslog("cMarkAdStandalone::CheckMarks(): stop (%d) start (%d) length %d", mark->position, mark->Next()->position, diff);
            if (diff > 3600) {
                dsyslog("cMarkAdStandalone::CheckMarks(): delete invalid pair");
                cMark *tmp = mark->Next()->Next();
                marks.Del(mark->Next());
                marks.Del(mark);
                mark = tmp;
                continue;
            }
        }

        // if no stop mark at the end, add one
        if (!inBroadCast || gotendmark) {  // in this case we will add a stop mark at the end of the recording
            if (((mark->type & 0x0F) == MT_START) && (!mark->Next())) {      // delete start mark at the end
                if (marks.GetFirst()->position != mark->position) {        // do not delete start mark
                    dsyslog("cMarkAdStandalone::CheckMarks(): START mark at the end, deleting %i", mark->position);
                    marks.Del(mark);
                    break;
                }
            }
        }
        mark = mark->Next();
    }

// delete logo and border marks if we have channel marks
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckMarks(): delete logo marks if we have channel or border marks");
    DebugMarks();     //  only for debugging
    cMark *channelStart = marks.GetNext(0, MT_CHANNELSTART);
    cMark *channelStop = marks.GetNext(0, MT_CHANNELSTOP);
    cMark *hborderStart = marks.GetNext(0, MT_HBORDERSTART);
    cMark *hborderStop = marks.GetNext(0, MT_HBORDERSTOP);
    cMark *vborderStart = marks.GetNext(0, MT_VBORDERSTART);
    cMark *vborderStop = marks.GetNext(0, MT_VBORDERSTOP);
    if (hborderStart && hborderStop) {
        int hDelta = (hborderStop->position - hborderStart->position) / macontext.Video.Info.framesPerSecond;
        if (hDelta < 120) {
            dsyslog("cMarkAdStandalone::CheckMarks(): found hborder stop/start, but distance %d too short, try if there is a next pair", hDelta);
            hborderStart = marks.GetNext(hborderStart->position, MT_HBORDERSTART);
            hborderStop = marks.GetNext(hborderStop->position, MT_HBORDERSTOP);
        }
    }
    if (vborderStart && vborderStop) {
        int vDelta = (vborderStop->position - vborderStart->position) / macontext.Video.Info.framesPerSecond;
        if (vDelta < 120) {
            dsyslog("cMarkAdStandalone::CheckMarks(): found vborder stop/start, but distance %d too short, try if there is a next pair", vDelta);
            vborderStart = marks.GetNext(vborderStart->position, MT_VBORDERSTART);
            vborderStop = marks.GetNext(vborderStop->position, MT_VBORDERSTOP);
        }
    }
    if ((channelStart && channelStop) || (hborderStart && hborderStop) || (vborderStart && vborderStop)) {
        mark = marks.GetFirst();
        while (mark) {
            if (mark != marks.GetFirst()) {
                if (mark == marks.GetLast()) break;
                if ((mark->type == MT_LOGOSTART) || (mark->type == MT_LOGOSTOP)) {
                    cMark *tmp = mark;
                    mark = mark->Next();
                    dsyslog("cMarkAdStandalone::CheckMarks(): delete logo mark (%i)", tmp->position);
                    marks.Del(tmp);
                    continue;
                }
            }
            mark = mark->Next();
        }
    }

// delete all black sceen marks expect start or end mark
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckMarks(): delete invalid black sceen marks");
    DebugMarks();     //  only for debugging
    mark = marks.GetFirst();
    while (mark) {
        if (mark != marks.GetFirst()) {
            if (mark == marks.GetLast()) break;
            if ((mark->type & 0xF0) == MT_BLACKCHANGE) {
                cMark *tmp = mark;
                mark = mark->Next();
                dsyslog("cMarkAdStandalone::CheckMarks(): delete black screen mark (%i)", tmp->position);
                marks.Del(tmp);
                continue;
            }
        }
        mark = mark->Next();
    }

// delete very short logo stop/start pairs
// contains start mark, do not delete
// diff 880 lengthAfter 203
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckMarks(): delete very short logo stop/start pairs");
    DebugMarks();     //  only for debugging
    mark = marks.GetFirst();
    while (mark) {
        if ((mark->type == MT_LOGOSTOP) && mark->Next() && mark->Next()->type == MT_LOGOSTART) {
            int diff = 1000 * (mark->Next()->position - mark->position) /  macontext.Video.Info.framesPerSecond;
            if (diff < 520 ) { // do not increase because of very short real logo interuption between broacast and preview, changed from 1000 to 920 to 520
                if (mark->Next()->Next() && (mark->Next()->Next()->type == MT_LOGOSTOP)) {
                    int lengthAfter = (mark->Next()->Next()->position - mark->Next()->position) /  macontext.Video.Info.framesPerSecond;
                    if (lengthAfter < 203) {  // do not delete a short stop/start before a long broadcast part, this pair contains start mark,
                        cMark *tmp = mark->Next()->Next();
                        dsyslog("cMarkAdStandalone::CheckMarks(): very short logo stop (%d) and logo start (%d) pair, diff %dms, length after %ds, deleting", mark->position, mark->Next()->position, diff, lengthAfter);
                        marks.Del(mark->Next());
                        marks.Del(mark);
                        mark = tmp;
                        continue;
                    }
                    else dsyslog("cMarkAdStandalone::CheckMarks(): logo stop (%d) and logo start (%d) pair, diff %dms, length %ds, long broadcast after, this can be a start mark", mark->position, mark->Next()->position, diff, lengthAfter);
                }
            }
            else dsyslog("cMarkAdStandalone::CheckMarks(): logo stop (%d) and logo start (%d) pair, diff %dms long enough", mark->position, mark->Next()->position, diff);
        }
        mark = mark->Next();
    }

// delete short START STOP logo marks because they are previews in the advertisement
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckMarks(): detect previews in advertisement");
    DebugMarks();     //  only for debugging
    mark = marks.GetFirst();
    while (mark) {
// check logo marks
        if ((mark->type == MT_LOGOSTART) && (mark->position != marks.GetFirst()->position)) {  // not start or end mark
            cMark *stopMark = marks.GetNext(mark->position, MT_LOGOSTOP);
            if (stopMark && (stopMark->type == MT_LOGOSTOP) && (stopMark->position != marks.GetLast()->position)) { // next logo stop mark not end mark
                cMark *stopBefore = marks.GetPrev(mark->position, MT_LOGOSTOP);
                cMark *startAfter= marks.GetNext(stopMark->position, MT_LOGOSTART);
                if (stopBefore && startAfter) {  // if advertising before is long this is the really the next start mark
                    int lengthAdBefore = static_cast<int> (1000 * (mark->position - stopBefore->position) / macontext.Video.Info.framesPerSecond);
                    int lengthAdAfter = static_cast<int> (1000 * (startAfter->position - stopMark->position) / macontext.Video.Info.framesPerSecond);
                    int lengthPreview = static_cast<int> ((stopMark->position - mark->position) / macontext.Video.Info.framesPerSecond);
                    dsyslog("cMarkAdStandalone::CheckMarks(): start (%d) stop (%d): length %ds, length ad before %dms, length ad after %dms",
                                                                                             mark->position, stopMark->position, lengthPreview, lengthAdBefore, lengthAdAfter);
                    if ((lengthAdBefore >= 1360) || (lengthAdAfter >= 2160)) {  // check if we have ad before or after preview. if not it is a logo detection failure
                                                                                // changed from 1400 to 1360
                                                                                // changed from 3200 to 2160
                        if ((lengthAdBefore >= 520) && (lengthAdBefore <= 585000) && (lengthAdAfter >= 520)) { // if advertising before is long this is the really the next start mark
                                                                                                                 // previews can be at start of advertising (e.g. DMAX)
                                                                                                                 // before max changed from 500000 to 560000 to 585000
                                                                                                                 // before min changed from 7000 to 5000 to 1000 to 920 to 520
                                                                                                                 // found very short logo invisible betweewn broascast and first preview
                                                                                                                 // after min changed from 2020 to 1520 to 1200 to 840 to 560 to 520
                            if (lengthPreview <= 111) {  // changed from 110 to 111
                                isyslog("found preview between logo mark (%d) and logo mark (%d) in advertisement, deleting marks", mark->position, stopMark->position);
                                cMark *tmp = startAfter;
                                marks.Del(mark);
                                marks.Del(stopMark);
                                mark = tmp;
                                continue;
                            }
                            else dsyslog("cMarkAdStandalone::CheckMarks(): no preview between (%d) and (%d), length %ds not valid",
                                                                                                                                mark->position, mark->Next()->position, lengthPreview);
                        }
                        else dsyslog("cMarkAdStandalone::CheckMarks(): no preview between (%d) and (%d), length advertising before %ds or after %dms is not valid",
                                                                                                    mark->position, mark->Next()->position, lengthAdBefore, lengthAdAfter);
                    }
                    else dsyslog("cMarkAdStandalone::CheckMarks(): not long enought ad before and after preview, maybe logo detection failure");
                }
                else dsyslog("cMarkAdStandalone::CheckMarks(): no preview because no MT_LOGOSTOP before or MT_LOGOSTART after found");
            }
        }
        mark = mark->Next();
    }

// delete short START STOP hborder marks because they are advertisement in the advertisement
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckMarks(): check border marks");
    DebugMarks();     //  only for debugging
    mark = marks.GetFirst();
    while (mark) {
        if ((mark->type == MT_HBORDERSTART) && (mark->position != marks.GetFirst()->position) && mark->Next()) {  // not start or end mark
            cMark *bStop = marks.GetNext(mark->position, MT_HBORDERSTOP);
            if (bStop && (bStop->position != marks.GetLast()->position)) { // next mark not end mark
                int lengthAd = static_cast<int> ((bStop->position - mark->position) / macontext.Video.Info.framesPerSecond);
                if (lengthAd < 130) { // increased from 70 to 130
                    isyslog("found advertisement of length %is between hborder mark (%i) and hborder mark (%i), deleting marks", lengthAd, mark->position, bStop->position);
                    cMark *logoStart = marks.GetNext(mark->position, MT_LOGOSTART);
                    if (logoStart && (logoStart->position <= bStop->position)) { // if there is a logo start between hborder start and bhorder end, it is the logo start of the preview, this is invalid
                        dsyslog("cMarkAdStandalone::CheckMarks(): invalid logo start mark between hborder start/stop, delete (%d)", logoStart->position);
                        marks.Del(logoStart);
                    }
                    cMark *tmp = mark;
                    mark = mark->Next();  // this can be the border stop mark or any other mark in between
                    if (mark->position == bStop->position) mark = mark->Next();  // there can be other marks in between
                    marks.Del(tmp);
                    marks.Del(bStop);
                    continue;
                }
            }
        }
        mark = mark->Next();
    }

// check start marks
// check for short start/stop pairs at the start
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckMarks(): check for short start/stop pairs at start");
    DebugMarks();     //  only for debugging
    mark = marks.GetFirst(); // this is the start mark
    if (mark) {
        cMark *markStop = marks.GetNext(mark->position, MT_STOP, 0x0F);
        if (markStop) {
            int maxDiff;
            if (mark->type <= MT_NOBLACKSTART) maxDiff = 96;  // do not trust weak marks
            else maxDiff = 8;
            int diffStop = (markStop->position - mark->position) / macontext.Video.Info.framesPerSecond; // length of the first broadcast part
            dsyslog("cMarkAdStandalone::CheckMarks(): first broadcast length %ds from (%d) to (%d)", diffStop, mark->position, markStop->position);
            if (diffStop <= maxDiff) {
                dsyslog("cMarkAdStandalone::CheckMarks(): short STOP/START/STOP sequence at start, delete first pair");
                marks.Del(mark->position);
                marks.Del(markStop->position);
            }
        }
    }

// check blackscreen and assumed end mark
// check for better end mark not very far away from assuemd end
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckMarks(): check for near better end mark in case of recording length is too big");
    DebugMarks();     //  only for debugging
    mark = marks.GetLast();
    if (mark && ((mark->type & 0xF0) < MT_CHANNELCHANGE)) {  // trust only channel marks and better
        int maxBeforeAssumed;           // max 5 min before assumed stop
        switch(mark->type) {
            case MT_ASSUMEDSTOP:
                maxBeforeAssumed = 389; // try hard to get a better end mark
                break;
            case MT_NOBLACKSTOP:
                maxBeforeAssumed = 351; // try a litte more to get end mark, changed from 389 to 351
                break;
            case MT_LOGOSTOP:
                maxBeforeAssumed = 306;
                break;
            default:
                maxBeforeAssumed = 300;                               // max 5 min before assumed stop
                break;
        }

        cMark *markPrev = marks.GetPrev(mark->position);
        if (markPrev) {
            int diffStart = (mark->position - markPrev->position) / macontext.Video.Info.framesPerSecond; // length of the last broadcast part, do not check it only depends on after timer
            dsyslog("cMarkAdStandalone::CheckMarks(): last broadcast length %ds from (%d) to (%d)", diffStart, markPrev->position, mark->position);
            if (diffStart >= 15) { // changed from 17 to 15
                cMark *markStop = marks.GetPrev(markPrev->position);
                if (markStop) {
                    int diffStop = (markPrev->position - markStop->position) / macontext.Video.Info.framesPerSecond; // distance of the logo stop/start pair before last pair
                    dsyslog("cMarkAdStandalone::CheckMarks(): last advertising length %ds from (%d) to (%d)", diffStop, markStop->position, markPrev->position);
                    if ((diffStop > 2) && (diffStop <= 163)) { // changed from 0 to 2 to avoid move to logo detection failure
                        if ((mark->type != MT_LOGOSTOP) || (diffStop < 11) || (diffStop > 12)) { // ad from 11s to 12s can be undetected info logo at the end (SAT.1 or RTL2)
                            int iStopA = marks.GetFirst()->position + macontext.Video.Info.framesPerSecond * (length + macontext.Config->astopoffs);  // we have to recalculate iStopA
                            int diffAssumed = (iStopA - markStop->position) / macontext.Video.Info.framesPerSecond; // distance from assumed stop
                            dsyslog("cMarkAdStandalone::CheckMarks(): last stop mark (%d) %ds before assumed stop (%d)", markStop->position, diffAssumed, iStopA);
                            if (diffAssumed <= maxBeforeAssumed) {
                                dsyslog("cMarkAdStandalone::CheckMarks(): use stop mark before as end mark, assume too big recording length");
                                marks.Del(mark->position);
                                marks.Del(markPrev->position);
                            }
                        }
                    }
                }
            }
        }
    }

// delete short START STOP logo marks because they are previes not detected above or due to next broadcast
// delete short STOP START logo marks because they are logo detection failure
// delete short STOP START hborder marks because some channels display information in the border
// delete short STOP START vborder marks because they are from malfunction recording
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckMarks(): remove logo and hborder detection failure marks");
    DebugMarks();     //  only for debugging

    mark = marks.GetFirst();
    while (mark) {
        if ((mark->position > marks.GetFirst()->position) && (mark->type == MT_LOGOSTART) && mark->Next() && mark->Next()->type == MT_LOGOSTOP) {  // do not delete selected start mark
                                                                                                                                                   // next logo stop/start pair could be "Teletext ..." info
            int MARKDIFF = static_cast<int> (macontext.Video.Info.framesPerSecond * 38); // changed from 8 to 18 to 35 to 38
            double distance = (mark->Next()->position - mark->position) / macontext.Video.Info.framesPerSecond;
            if ((mark->Next()->position - mark->position) <= MARKDIFF) {
                isyslog("mark distance between logo START and STOP too short %.1fs, deleting (%i,%i)", distance, mark->position, mark->Next()->position);
                cMark *tmp = mark;
                mark = mark->Next()->Next();
                marks.Del(tmp->Next());
                marks.Del(tmp);
                continue;
            }
            else dsyslog("cMarkAdStandalone::CheckMarks(): mark distance between logo START and STOP %.1fs, keep (%i,%i)", distance, mark->position, mark->Next()->position);
        }
        if ((mark->type == MT_LOGOSTOP) && mark->Next() && mark->Next()->type == MT_LOGOSTART) {
            int MARKDIFF = static_cast<int> (macontext.Video.Info.framesPerSecond * 23);   // assume thre is shortest advertising, changed from 20s to 23s
            if ((mark->Next()->position - mark->position) <= MARKDIFF) {
                double distance = (mark->Next()->position - mark->position) / macontext.Video.Info.framesPerSecond;
                isyslog("mark distance between logo STOP and START too short (%.1fs), deleting %i,%i", distance, mark->position, mark->Next()->position);
                cMark *tmp = mark;
                mark = mark->Next()->Next();
                marks.Del(tmp->Next());
                marks.Del(tmp);
                continue;
            }
        }
        if ((mark->type == MT_HBORDERSTOP) && mark->Next() && mark->Next()->type == MT_HBORDERSTART) {
            int MARKDIFF = static_cast<int> (macontext.Video.Info.framesPerSecond * 20);  // increased from 15 to 20
            if ((mark->Next()->position - mark->position) <= MARKDIFF) {
                double distance = (mark->Next()->position - mark->position) / macontext.Video.Info.framesPerSecond;
                isyslog("mark distance between horizontal STOP and START too short (%.1fs), deleting %i,%i", distance, mark->position, mark->Next()->position);
                cMark *tmp = mark;
                mark = mark->Next()->Next();
                marks.Del(tmp->Next());
                marks.Del(tmp);
                continue;
            }
        }
        if ((mark->type == MT_VBORDERSTOP) && mark->Next() && mark->Next()->type == MT_VBORDERSTART) {
            int MARKDIFF = static_cast<int> (macontext.Video.Info.framesPerSecond * 2);
            if ((mark->Next()->position - mark->position) <= MARKDIFF) {
                double distance = (mark->Next()->position - mark->position) / macontext.Video.Info.framesPerSecond;
                isyslog("mark distance between vertical STOP and START too short (%.1fs), deleting %i,%i", distance, mark->position, mark->Next()->position);
                cMark *tmp = mark;
                mark = mark->Next()->Next();
                marks.Del(tmp->Next());
                marks.Del(tmp);
                continue;
            }
        }
        mark = mark->Next();
    }

// if we have a VPS events, move start and stop mark to VPS event
    LogSeparator();
    if (macontext.Config->useVPS) {
        LogSeparator();
        dsyslog("cMarkAdStandalone::CheckMarks(): apply VPS events");
        DebugMarks();     //  only for debugging
        if (ptr_cDecoder) {
            int vpsOffset = marks.LoadVPS(macontext.Config->recDir, "START:"); // VPS start mark
            if (vpsOffset >= 0) {
                isyslog("found VPS start event at offset %ds", vpsOffset);
                AddMarkVPS(vpsOffset, MT_START, false);
            }
            else dsyslog("cMarkAdStandalone::CheckMarks(): no VPS start event found");

            vpsOffset = marks.LoadVPS(macontext.Config->recDir, "PAUSE_START:");     // VPS pause start mark = stop mark
            if (vpsOffset >= 0) {
                isyslog("found VPS pause start event at offset %ds", vpsOffset);
                AddMarkVPS(vpsOffset, MT_STOP, true);
            }
            else dsyslog("cMarkAdStandalone::CheckMarks(): no VPS pause start event found");

            vpsOffset = marks.LoadVPS(macontext.Config->recDir, "PAUSE_STOP:");     // VPS pause stop mark = start mark
            if (vpsOffset >= 0) {
                isyslog("found VPS pause stop event at offset %ds", vpsOffset);
                AddMarkVPS(vpsOffset, MT_START, true);
            }
            else dsyslog("cMarkAdStandalone::CheckMarks(): no VPS pause stop event found");

            vpsOffset = marks.LoadVPS(macontext.Config->recDir, "STOP:");     // VPS stop mark
            if (vpsOffset >= 0) {
                isyslog("found VPS stop event at offset %ds", vpsOffset);
                AddMarkVPS(vpsOffset, MT_STOP, false);
            }
            else dsyslog("cMarkAdStandalone::CheckMarks(): no VPS stop event found");
        }
        else isyslog("VPS info usage requires --cDecoder");

// once again check marks
        mark = marks.GetFirst();
        while (mark) {
            if (((mark->type & 0x0F)==MT_START) && (mark->Next()) && ((mark->Next()->type & 0x0F)==MT_START)) {  // two start marks, delete second
                dsyslog("cMarkAdStandalone::CheckMarks(): start mark (%i) followed by start mark (%i) delete non VPS mark", mark->position, mark->Next()->position);
                if (mark->type == MT_VPSSTART) {
                    marks.Del(mark->Next());
                    continue;
                }
                if (mark->Next()->type == MT_VPSSTART) {
                    cMark *tmp=mark;
                    mark = mark->Next();
                    marks.Del(tmp);
                    continue;
                }
            }
            if (((mark->type & 0x0F)==MT_STOP) && (mark->Next()) && ((mark->Next()->type & 0x0F)==MT_STOP)) {  // two stop marks, delete second
                dsyslog("cMarkAdStandalone::CheckMarks(): stop mark (%i) followed by stop mark (%i) delete non VPS mark", mark->position, mark->Next()->position);
                if (mark->type == MT_VPSSTOP) {
                    marks.Del(mark->Next());
                    continue;
                }
                if (mark->Next()->type == MT_VPSSTOP) {
                    cMark *tmp=mark;
                    mark = mark->Next();
                    marks.Del(tmp);
                    continue;
                }
            }
            if (((mark->type & 0x0F) == MT_START) && (!mark->Next())) {      // delete start mark at the end
                if (marks.GetFirst()->position != mark->position) {        // do not delete start mark
                    dsyslog("cMarkAdStandalone::CheckMarks(): START mark at the end, deleting %i", mark->position);
                    marks.Del(mark);
                    break;
                }
            }
            mark = mark->Next();
        }
    }
    LogSeparator();
    dsyslog("cMarkAdStandalone::CheckMarks(): final marks:");
    DebugMarks();     //  only for debugging
    LogSeparator();
}


void cMarkAdStandalone::AddMarkVPS(const int offset, const int type, const bool isPause) {
    if (!ptr_cDecoder) return;
    int delta = macontext.Video.Info.framesPerSecond * MAXRANGE;
    int vpsFrame = recordingIndexMark->GetFrameFromOffset(offset * 1000);
    if (vpsFrame < 0) {
        dsyslog("cMarkAdStandalone::AddMarkVPS(): failed to get frame from offset %d", offset);
    }
    cMark *mark = NULL;
    char *comment = NULL;
    char *timeText = NULL;
    if (!isPause) {
        char *indexToHMSF = marks.IndexToHMSF(vpsFrame, &macontext);
        dsyslog("cMarkAdStandalone::AddMarkVPS(): found VPS %s at frame (%d) at %s", (type == MT_START) ? "start" : "stop", vpsFrame, indexToHMSF);
        FREE(strlen(indexToHMSF)+1, "indexToHMSF");
        free(indexToHMSF);
        mark = ((type == MT_START)) ? marks.GetNext(0, MT_START, 0x0F) : marks.GetPrev(INT_MAX, MT_STOP, 0x0F);
    }
    else {
        char *indexToHMSF = marks.IndexToHMSF(vpsFrame, &macontext);
        dsyslog("cMarkAdStandalone::AddMarkVPS(): found VPS %s at frame (%d) at %s", (type == MT_START) ? "pause start" : "pause stop", vpsFrame, indexToHMSF);
        FREE(strlen(indexToHMSF)+1, "indexToHMSF");
        free(indexToHMSF);
        mark = ((type == MT_START)) ? marks.GetAround(delta, vpsFrame, MT_START, 0x0F) :  marks.GetAround(delta, vpsFrame, MT_STOP, 0x0F);
    }
    if (!mark) {
        if (isPause) {
            dsyslog("cMarkAdStandalone::AddMarkVPS(): no mark found to replace with pause mark, add new mark");
            if (asprintf(&comment,"VPS %s (%d)%s", (type == MT_START) ? "pause start" : "pause stop", vpsFrame, (type == MT_START) ? "*" : "") == -1) comment=NULL;
            ALLOC(strlen(comment)+1, "comment");
            marks.Add((type == MT_START) ? MT_VPSSTART : MT_VPSSTOP, vpsFrame, comment);
            FREE(strlen(comment)+1,"comment");
            free(comment);
            return;
        }
        dsyslog("cMarkAdStandalone::AddMarkVPS(): found no mark found to replace");
        return;
    }
    if ( (type & 0x0F) != (mark->type & 0x0F)) return;

    timeText = marks.IndexToHMSF(mark->position, &macontext);
    if (timeText) {
        if ((mark->type > MT_LOGOCHANGE) && (mark->type != MT_RECORDINGSTART)) {  // keep strong marks, they are better than VPS marks
                                                                                  // for VPS recording we replace recording start mark
            dsyslog("cMarkAdStandalone::AddMarkVPS(): keep mark at frame (%d) type 0x%X at %s", mark->position, mark->type, timeText);
        }
        else {
            dsyslog("cMarkAdStandalone::AddMarkVPS(): mark to replace at frame (%d) type 0x%X at %s", mark->position, mark->type, timeText);
            if (asprintf(&comment,"VPS %s (%d), moved from mark (%d) type 0x%X at %s %s", (type == MT_START) ? "start" : "stop", vpsFrame, mark->position, mark->type, timeText, (type == MT_START) ? "*" : "") == -1) comment=NULL;
            ALLOC(strlen(comment)+1, "comment");
            dsyslog("cMarkAdStandalone::AddMarkVPS(): delete mark on position (%d)", mark->position);
            marks.Del(mark->position);
            marks.Add((type == MT_START) ? MT_VPSSTART : MT_VPSSTOP, vpsFrame, comment);
            FREE(strlen(comment)+1,"comment");
            free(comment);
            if ((type == MT_START) && !isPause) {   // delete all marks before vps start
                marks.DelWeakFromTo(0, vpsFrame, 0xFF);
            }
            else if ((type == MT_STOP) && isPause) {  // delete all marks between vps start and vps pause start
                cMark *startVPS = marks.GetFirst();
                if (startVPS && (startVPS->type == MT_VPSSTART)) {
                    marks.DelWeakFromTo(startVPS->position, vpsFrame, MT_VPSCHANGE);
                }
            }
        }
        FREE(strlen(timeText)+1, "indexToHMSF");
        free(timeText);
    }
}


void cMarkAdStandalone::AddMark(sMarkAdMark *mark) {
    if (!mark) return;
    if (!mark->type) return;
    if ((macontext.Config) && (macontext.Config->logoExtraction != -1)) return;
    if (gotendmark) return;

    char *comment = NULL;
    switch (mark->type) {
        case MT_ASSUMEDSTART:
            if (asprintf(&comment, "assuming start (%i)*", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_ASSUMEDSTOP:
            if (asprintf(&comment, "assuming stop (%i)", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_NOBLACKSTART:
            if (asprintf(&comment, "detected end of black screen (%i)*", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_NOBLACKSTOP:
            if (asprintf(&comment, "detected start of black screen (%i)", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_LOGOSTART:
            if (asprintf(&comment, "detected logo start (%i)*", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_LOGOSTOP:
            if (asprintf(&comment, "detected logo stop (%i)", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_HBORDERSTART:
            if (asprintf(&comment, "detected start of horiz. borders (%i)*", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_HBORDERSTOP:
            if (asprintf(&comment, "detected stop of horiz. borders (%i)", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_VBORDERSTART:
            if (asprintf(&comment, "detected start of vert. borders (%i)*", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_VBORDERSTOP:
            if (asprintf(&comment, "detected stop of vert. borders (%i)", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_ASPECTSTART:
            if (!mark->AspectRatioBefore.num) {
                if (asprintf(&comment, "aspectratio start with %i:%i (%i)*", mark->AspectRatioAfter.num, mark->AspectRatioAfter.den, mark->position) == -1) comment = NULL;
                ALLOC(strlen(comment)+1, "comment");
            }
            else {
                if (asprintf(&comment, "aspectratio change from %i:%i to %i:%i (%i)*", mark->AspectRatioBefore.num, mark->AspectRatioBefore.den,
                         mark->AspectRatioAfter.num, mark->AspectRatioAfter.den, mark->position) == -1) comment = NULL;
                ALLOC(strlen(comment)+1, "comment");
                if ((macontext.Config->autoLogo > 0) &&( mark->position > 0) && bDecodeVideo) {
                    isyslog("logo detection reenabled at frame (%d), trying to find a logo from this position", mark->position);
                    macontext.Video.Options.ignoreLogoDetection = false;
                    macontext.Video.Options.ignoreBlackScreenDetection = false;
                }
            }
            break;
        case MT_ASPECTSTOP:
            if (asprintf(&comment, "aspectratio change from %i:%i to %i:%i (%i)", mark->AspectRatioBefore.num, mark->AspectRatioBefore.den,
                     mark->AspectRatioAfter.num, mark->AspectRatioAfter.den, mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            if ((macontext.Config->autoLogo > 0) && (mark->position > 0) && bDecodeVideo) {
                isyslog("logo detection reenabled at frame (%d), trying to find a logo from this position", mark->position);
                macontext.Video.Options.ignoreLogoDetection = false;
                macontext.Video.Options.ignoreBlackScreenDetection = false;
            }
            break;
        case MT_CHANNELSTART:
            macontext.Audio.Info.channelChange = true;
            if (asprintf(&comment, "audio channel change from %i to %i (%i)*", mark->channelsBefore, mark->channelsAfter, mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_CHANNELSTOP:
            if ((mark->position > chkSTART) && (mark->position < iStopA / 2) && !macontext.Audio.Info.channelChange) {
                dsyslog("cMarkAdStandalone::AddMark(): first audio channel change is after chkSTART, disable logo/border/aspect detection now");
                if (iStart == 0) marks.DelWeakFromTo(marks.GetFirst()->position, INT_MAX, MT_CHANNELCHANGE); // only if we heve selected a start mark
                bDecodeVideo = false;
                macontext.Video.Options.ignoreAspectRatio = true;
                macontext.Video.Options.ignoreLogoDetection = true;
                macontext.Video.Options.ignoreBlackScreenDetection = true;
            }
            macontext.Audio.Info.channelChange = true;
            if (asprintf(&comment, "audio channel change from %i to %i (%i)", mark->channelsBefore, mark->channelsAfter, mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_RECORDINGSTART:
            if (asprintf(&comment, "start of recording (%i)", mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        case MT_RECORDINGSTOP:
            if (asprintf(&comment, "stop of recording (%i)",mark->position) == -1) comment = NULL;
            ALLOC(strlen(comment)+1, "comment");
            break;
        default:
            dsyslog("cMarkAdStandalone::AddMark(): unknown mark type 0x%X", mark->type);
    }

    cMark *prev = marks.GetLast();
    while (prev) { // we do not want blackscreen marks
        if ((prev->type & 0xF0) == MT_BLACKCHANGE) prev = prev->Prev();
        else break;
    }
    if (prev) {
        if (((prev->type & 0x0F) == (mark->type & 0x0F)) && ((prev->type & 0xF0) != (mark->type & 0xF0))) { // do not delete same mark type
            int markDiff = 30;
            if (iStart != 0) markDiff = 2;  // before chkStart: let more marks untouched, we need them for start detection
            if (restartLogoDetectionDone) markDiff = 15; // we are in the end part, keep more marks to detect best end mark
            int diff = (abs(mark->position - prev->position)) / macontext.Video.Info.framesPerSecond;
            if (diff < markDiff) {
                if (prev->type > mark->type) {
                    isyslog("previous mark (%i) type 0x%X stronger than actual mark with distance %ds, deleting (%i) type 0x%X", prev->position, prev->type, diff, mark->position, mark->type);
                    if ((mark->type & 0xF0) == MT_BLACKCHANGE) {
                        blackMarks.Add(mark->type, mark->position, NULL, false); // add mark to blackscreen list
                    }
                    if (comment) {
                        FREE(strlen(comment)+1, "comment");
                        free(comment);
                    }
                    return;
                }
                else {
                    isyslog("actual mark (%i) type 0x%X stronger then previous mark with distance %ds, deleting %i type 0x%X", mark->position, mark->type, diff, prev->position, prev->type);
                    if ((prev->type & 0xF0) == MT_BLACKCHANGE) {
                        blackMarks.Add(prev->type, prev->position, NULL, false); // add mark to blackscreen list
                    }
                    marks.Del(prev);
                }
            }
        }
    }

// set inBroadCast status
    if ((mark->type & 0xF0) != MT_BLACKCHANGE){ //  dont use BLACKSCEEN to detect if we are in broadcast
        if (!((mark->type <= MT_ASPECTSTART) && (marks.GetPrev(mark->position, MT_CHANNELSTOP) && marks.GetPrev(mark->position, MT_CHANNELSTART)))) { // if there are MT_CHANNELSTOP and MT_CHANNELSTART marks, wait for MT_CHANNELSTART
            if ((mark->type & 0x0F) == MT_START) {
                inBroadCast = true;
            }
            else {
                inBroadCast = false;
            }
        }
    }

// add mark
    char *indexToHMSF = marks.IndexToHMSF(mark->position, &macontext);
    if (indexToHMSF) {
        if (comment) {
            if ((mark->type & 0xF0) == MT_BLACKCHANGE) dsyslog("%s at %s inBroadCast: %i",comment, indexToHMSF, inBroadCast);
            else isyslog("%s at %s inBroadCast: %i",comment, indexToHMSF, inBroadCast);
        }
        FREE(strlen(indexToHMSF)+1, "indexToHMSF");
        free(indexToHMSF);
    }
    if ((mark->type & 0xF0) == MT_BLACKCHANGE) blackMarks.Add(mark->type, mark->position, NULL, inBroadCast);
    marks.Add(mark->type, mark->position, comment, inBroadCast);
    if (comment) {
        FREE(strlen(comment)+1, "comment");
        free(comment);
    }

// save marks
    if (iStart == 0) marks.Save(directory, &macontext, false);  // save after start mark is valid
}


// save currect content of the frame buffer to /tmp
// if path and suffix is set, this will set as target path and file name suffix
//
#if defined(DEBUG_LOGO_DETECT_FRAME_CORNER) || defined(DEBUG_MARK_FRAMES)
void cMarkAdStandalone::SaveFrame(const int frame, const char *path, const char *suffix) {
    if (!macontext.Video.Info.height) {
        dsyslog("cMarkAdStandalone::SaveFrame(): macontext.Video.Info.height not set");
        return;
    }
    if (!macontext.Video.Info.width) {
        dsyslog("cMarkAdStandalone::SaveFrame(): macontext.Video.Info.width not set");
        return;
    }
    if (!macontext.Video.Data.valid) {
        dsyslog("cMarkAdStandalone::SaveFrame():  macontext.Video.Data.valid not set");
        return;
    }
    char szFilename[1024];

    for (int plane = 0; plane < PLANES; plane++) {
        int height;
        int width;
        if (plane == 0) {
            height = macontext.Video.Info.height;
            width  = macontext.Video.Info.width;
        }
        else {
            height = macontext.Video.Info.height / 2;
            width  = macontext.Video.Info.width  / 2;
        }
        // set path and file name
        if (path && suffix) sprintf(szFilename, "%s/frame%06d_P%d_%s.pgm", path, frame, plane, suffix);
        else sprintf(szFilename, "/tmp/frame%06dfull_P%d.pgm", frame, plane);
        // Open file
        FILE *pFile = fopen(szFilename, "wb");
        if (pFile == NULL) {
            dsyslog("cMarkAdStandalone::SaveFrame(): open file %s failed", szFilename);
            return;
        }
        // Write header
        fprintf(pFile, "P5\n%d %d\n255\n", width, height);
        // Write pixel data
        for (int line = 0; line < height; line++) {
            if (fwrite(&macontext.Video.Data.Plane[plane][line * macontext.Video.Data.PlaneLinesize[plane]], 1, width, pFile)) {};
        }
        // Close file
        fclose(pFile);
    }
}
#endif


void cMarkAdStandalone::CheckIndexGrowing()
{
    // Here we check if the index is more
    // advanced than our framecounter.
    // If not we wait. If we wait too much,
    // we discard this check...

#define WAITTIME 15

    if (!indexFile) {
        dsyslog("cMarkAdStandalone::CheckIndexGrowing(): no index file found");
        return;
    }
    if (macontext.Config->logoExtraction != -1) {
        return;
    }
    if (sleepcnt >= 2) {
        dsyslog("slept too much");
        return; // we already slept too much
    }
    if (ptr_cDecoder) framecnt1 = ptr_cDecoder->GetFrameNumber();
    bool notenough = true;
    do {
        struct stat statbuf;
        if (stat(indexFile,&statbuf) == -1) {
            return;
        }

        int maxframes = statbuf.st_size / 8;
        if (maxframes < (framecnt1 + 200)) {
            if ((difftime(time(NULL), statbuf.st_mtime)) >= WAITTIME) {
                if (length && startTime) {
                    time_t endRecording = startTime + (time_t) length;
                    if (time(NULL) > endRecording) {
                        // no markad during recording
//                        dsyslog("cMarkAdStandalone::CheckIndexGrowing(): assuming old recording, now > startTime + length");
                        return;
                    }
                    else {
                        sleepcnt = 0;
                        if (!iwaittime) {
                            dsyslog("cMarkAdStandalone::CheckIndexGrowing(): startTime %s length %d", strtok(ctime(&startTime), "\n"), length);
                            dsyslog("cMarkAdStandalone::CheckIndexGrowing(): expected end: %s", strtok(ctime(&endRecording), "\n"));
                            esyslog("recording interrupted, waiting for continuation...");
                        }
                        iwaittime += WAITTIME;
                    }
                }
                else {
                    dsyslog("cMarkAdStandalone::CheckIndexGrowing(): no length and startTime");
                    return;
                }
            }
            unsigned int sleeptime = WAITTIME;
            time_t sleepstart = time(NULL);
            double slepttime = 0;
            while ((unsigned int)slepttime < sleeptime) {
                while (sleeptime > 0) {
                    macontext.Info.isRunningRecording = true;
                    unsigned int ret = sleep(sleeptime); // now we sleep and hopefully the index will grow
                    if ((errno) && (ret)) {
                        if (abortNow) return;
                        esyslog("got errno %i while waiting for new data", errno);
                        if (errno != EINTR) return;
                    }
                    sleeptime = ret;
                }
                slepttime = difftime(time(NULL), sleepstart);
                if (slepttime < WAITTIME) {
                    esyslog("what's wrong with your system? we just slept %.0fs", slepttime);
                }
            }
            waittime += static_cast<int> (slepttime);
            sleepcnt++;
            if (sleepcnt >= 2) {
                esyslog("no new data after %is, skipping wait!", waittime);
                notenough = false; // something went wrong?
            }
        }
        else {
            if (iwaittime) {
                esyslog("resuming after %is of interrupted recording, marks can be wrong now!", iwaittime);
            }
            iwaittime = 0;
            sleepcnt = 0;
            notenough = false;
        }
    }
    while (notenough);
    return;
}


bool cMarkAdStandalone::ProcessMark2ndPass(cMarkAdOverlap *overlap, cMark **mark1, cMark **mark2) {
    if (!ptr_cDecoder) return false;
    if (!mark1) return false;
    if (!*mark1) return false;
    if (!mark2) return false;
    if (!*mark2) return false;


    sOverlapPos *ptr_OverlapPos = NULL;
    Reset();

// calculate overlap check positions
#define OVERLAP_CHECK_BEFORE 120  // start 2 min before stop mark
    int fRangeBegin = (*mark1)->position - (macontext.Video.Info.framesPerSecond * OVERLAP_CHECK_BEFORE);
    if (fRangeBegin < 0) fRangeBegin = 0;                    // not before beginning of broadcast
    fRangeBegin = recordingIndexMark->GetIFrameBefore(fRangeBegin);
    if (fRangeBegin < 0) {
        dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): GetIFrameBefore failed for frame (%d)", fRangeBegin);
        return false;
    }
#define OVERLAP_CHECK_AFTER 300  // start 5 min after start mark
    int fRangeEnd = (*mark2)->position + (macontext.Video.Info.framesPerSecond * OVERLAP_CHECK_AFTER);

    cMark *prevStart = marks.GetPrev((*mark1)->position, MT_START, 0x0F);
    if (prevStart) {
        if (fRangeBegin <= (prevStart->position + ((OVERLAP_CHECK_AFTER + 1) * macontext.Video.Info.framesPerSecond))) { // previous start mark less than OVERLAP_CHECK_AFTER away, prevent overlapping check
            dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): previous stop mark at (%d) very near, unable to check overlap", prevStart->position);
            return false;
        }
    }

    cMark *nextStop = marks.GetNext((*mark2)->position, MT_STOP, 0x0F);
    if (nextStop) {
        if (nextStop->position != marks.GetLast()->position) {
            if (fRangeEnd >= (nextStop->position - ((OVERLAP_CHECK_BEFORE + OVERLAP_CHECK_AFTER + 1) * macontext.Video.Info.framesPerSecond))) { // next start mark less than OVERLAP_CHECK_AFTER + OVERLAP_CHECK_BEFORE away, prevent overlapping check
                fRangeEnd = nextStop->position - ((OVERLAP_CHECK_BEFORE + 1) * macontext.Video.Info.framesPerSecond);
                if (fRangeEnd <= (*mark2)->position) {
                    dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): next stop mark at (%d) very near, unable to check overlap", nextStop->position);
                    return false;
                }
                dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): next stop mark at (%d) to near, reduce check end position", nextStop->position);
            }
        }
        if (nextStop->position < fRangeEnd) fRangeEnd = nextStop->position;  // do not check after next stop mark position
    }

    dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): preload from frame       (%5d) to (%5d)", fRangeBegin, (*mark1)->position);
    dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): compare with frames from (%5d) to (%5d)", (*mark2)->position, fRangeEnd);

// seek to start frame of overlap check
    char *indexToHMSF = marks.IndexToHMSF(fRangeBegin, &macontext);
    if (indexToHMSF) {
        dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): start check %ds before at frame (%d) and start overlap check at %s", OVERLAP_CHECK_BEFORE, fRangeBegin, indexToHMSF);
        FREE(strlen(indexToHMSF)+1, "indexToHMSF");
        free(indexToHMSF);
    }
    if (!ptr_cDecoder->SeekToFrame(&macontext, fRangeBegin)) {
        esyslog("could not seek to frame (%i)", fRangeBegin);
        return false;
    }

// get iFrame count of range to check for overlap
    int iFrameCount = recordingIndexMark->GetIFrameRangeCount(fRangeBegin, (*mark1)->position);
    if (iFrameCount < 0) {
        dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): GetIFrameRangeCount failed at range (%d,%d))", fRangeBegin, (*mark1)->position);
        return false;
    }
    dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): %d i-frames to preload between start of check (%d) and stop mark (%d)", iFrameCount, fRangeBegin, (*mark1)->position);

// preload frames before stop mark
    while (ptr_cDecoder->GetFrameNumber() <= (*mark1)->position ) {
        if (abortNow) return false;
        if (!ptr_cDecoder->GetNextPacket()) {
            dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): GetNextPacket failed at frame (%d)", ptr_cDecoder->GetFrameNumber());
            return false;
        }
        if (!ptr_cDecoder->IsVideoPacket()) continue;
        if (!ptr_cDecoder->GetFrameInfo(&macontext, false)) {
            if (ptr_cDecoder->IsVideoIFrame())  // if we have interlaced video this is expected, we have to read the next half picture
                tsyslog("cMarkAdStandalone::ProcessMark2ndPass() before mark GetFrameInfo failed at frame (%d)", ptr_cDecoder->GetFrameNumber());
            continue;
        }
        if (ptr_cDecoder->IsVideoIFrame()) {
            ptr_OverlapPos = overlap->Process(ptr_cDecoder->GetFrameNumber(), iFrameCount, true, (macontext.Info.vPidType == MARKAD_PIDTYPE_VIDEO_H264));
        }
    }

// seek to iFrame before start mark
    fRangeBegin = recordingIndexMark->GetIFrameBefore((*mark2)->position);
    if (fRangeBegin <= 0) {
        dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): GetIFrameBefore failed for frame (%d)", fRangeBegin);
        return false;
    }
    if (fRangeBegin <  ptr_cDecoder->GetFrameNumber()) fRangeBegin = ptr_cDecoder->GetFrameNumber(); // on very short stop/start pairs we have no room to go before start mark
    indexToHMSF = marks.IndexToHMSF(fRangeBegin, &macontext);
    if (indexToHMSF) {
        dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): seek forward to iFrame (%d) at %s before start mark (%d) and start overlap check", fRangeBegin, indexToHMSF, (*mark2)->position);
        FREE(strlen(indexToHMSF)+1, "indexToHMSF");
        free(indexToHMSF);
    }
    if (!ptr_cDecoder->SeekToFrame(&macontext, fRangeBegin)) {
        esyslog("could not seek to frame (%d)", fRangeBegin);
        return false;
    }

    iFrameCount = recordingIndexMark->GetIFrameRangeCount(fRangeBegin, fRangeEnd) - 2;
    if (iFrameCount < 0) {
            dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): GetIFrameRangeCount failed at range (%d,%d))", fRangeBegin, (*mark1)->position);
            return false;
    }
    char *indexToHMSFbegin = marks.IndexToHMSF(fRangeBegin, &macontext);
    char *indexToHMSFend = marks.IndexToHMSF(fRangeEnd, &macontext);
    if (indexToHMSFbegin && indexToHMSFend) {
        dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): process overlap detection between frame (%d) at %s and frame (%d) at %s", fRangeBegin, indexToHMSFbegin, fRangeEnd, indexToHMSFend);
    }
    if (indexToHMSFbegin) {
        FREE(strlen(indexToHMSFbegin)+1, "indexToHMSF");
        free(indexToHMSFbegin);
    }
    if (indexToHMSFend) {
        FREE(strlen(indexToHMSFend)+1, "indexToHMSF");
        free(indexToHMSFend);
    }

// process frames after start mark and detect overlap
    while (ptr_cDecoder->GetFrameNumber() <= fRangeEnd ) {
        if (abortNow) return false;
        if (!ptr_cDecoder->GetNextPacket()) {
            dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): GetNextPacket failed at frame (%d)", ptr_cDecoder->GetFrameNumber());
            return false;
        }
        if (!ptr_cDecoder->IsVideoPacket()) continue;
        if (!ptr_cDecoder->GetFrameInfo(&macontext, false)) {
            if (ptr_cDecoder->IsVideoIFrame())
                tsyslog("cMarkAdStandalone::ProcessMark2ndPass() after mark GetFrameInfo failed at frame (%d)", ptr_cDecoder->GetFrameNumber());
            continue;
        }
        if (ptr_cDecoder->IsVideoIFrame()) {
            ptr_OverlapPos = overlap->Process(ptr_cDecoder->GetFrameNumber(), iFrameCount, false, (macontext.Info.vPidType==MARKAD_PIDTYPE_VIDEO_H264));
        }
        if (ptr_OverlapPos) {
            // found overlap
            char *indexToHMSFbefore = marks.IndexToHMSF(ptr_OverlapPos->frameNumberBefore, &macontext);
            char *indexToHMSFmark1 = marks.IndexToHMSF((*mark1)->position, &macontext);
            char *indexToHMSFmark2 = marks.IndexToHMSF((*mark2)->position, &macontext);
            char *indexToHMSFafter = marks.IndexToHMSF(ptr_OverlapPos->frameNumberAfter, &macontext);
            if (indexToHMSFbefore && indexToHMSFmark1 && indexToHMSFmark2 && indexToHMSFafter) {
                dsyslog("cMarkAdStandalone::ProcessMark2ndPass(): found overlap from (%6d) at %s to (%6d) at %s are identical with",
                            ptr_OverlapPos->frameNumberBefore, indexToHMSFbefore, (*mark1)->position, indexToHMSFmark1);
                dsyslog("cMarkAdStandalone::ProcessMark2ndPass():                    (%6d) at %s to (%6d) at %s",
                            (*mark2)->position, indexToHMSFmark2, ptr_OverlapPos->frameNumberAfter, indexToHMSFafter);
            }
            if (indexToHMSFbefore) {
                FREE(strlen(indexToHMSFbefore)+1, "indexToHMSF");
                free(indexToHMSFbefore);
            }
            if (indexToHMSFmark1) {
                FREE(strlen(indexToHMSFmark1)+1, "indexToHMSF");
                free(indexToHMSFmark1);
            }
            if (indexToHMSFmark2) {
                FREE(strlen(indexToHMSFmark2)+1, "indexToHMSF");
                free(indexToHMSFmark2);
            }
            if (indexToHMSFafter) {
                FREE(strlen(indexToHMSFafter)+1, "indexToHMSF");
                free(indexToHMSFafter);
            }
            *mark1 = marks.Move(&macontext, *mark1, ptr_OverlapPos->frameNumberBefore, "overlap");
            *mark2 = marks.Move(&macontext, *mark2, ptr_OverlapPos->frameNumberAfter, "overlap");
            marks.Save(directory, &macontext, false);
            return true;
        }
    }
    return false;
}


#ifdef DEBUG_MARK_FRAMES
void cMarkAdStandalone::DebugMarkFrames() {
    if (!ptr_cDecoder) return;

    ptr_cDecoder->Reset();
    cMark *mark = marks.GetFirst();
    if (!macontext.Config->fullDecode) {
        while (mark) {
            if (mark->position != recordingIndexMark->GetIFrameBefore(mark->position)) dsyslog("cMarkAdStandalone::DebugMarkFrames(): mark at (%d) type 0x%X is not a iFrame position", mark->position, mark->type);
            mark=mark->Next();
        }
    }

    mark = marks.GetFirst();
    if (!mark) return;

    int writePosition = mark->position;
    for (int i = 0; i < DEBUG_MARK_FRAMES; i++) {
        if (!macontext.Config->fullDecode) writePosition = recordingIndexMark->GetIFrameBefore(writePosition - 1);
        else writePosition--;
    }
    int writeOffset = -DEBUG_MARK_FRAMES;

    // read and decode all video frames, we want to be sure we have a valid decoder state, this is a debug function, we dont care about performance
    while(mark && (ptr_cDecoder->DecodeDir(directory))) {
        while(mark && (ptr_cDecoder->GetNextPacket())) {
            if (ptr_cDecoder->IsVideoPacket()) {
                if (ptr_cDecoder->GetFrameInfo(&macontext, macontext.Config->fullDecode)) {
                    if (ptr_cDecoder->GetFrameNumber() >= writePosition) {
                        dsyslog("cMarkAdStandalone::DebugMarkFrames(): mark at frame (%5d) type 0x%X, write frame (%5d)", mark->position, mark->type, writePosition);
                        if (writePosition == mark->position) {
                            if ((mark->type & 0x0F) == MT_START) SaveFrame(mark->position, directory, "START");
                            else if ((mark->type & 0x0F) == MT_STOP) SaveFrame(mark->position, directory, "STOP");
                                 else SaveFrame(mark->position, directory, "MOVED");
                        }
                        else {
                            SaveFrame(writePosition, directory, (writePosition < mark->position) ? "BEFORE" : "AFTER");
                        }
                        if (!macontext.Config->fullDecode) writePosition = recordingIndexMark->GetIFrameAfter(writePosition + 1);
                        else writePosition++;
                        if (writeOffset >= DEBUG_MARK_FRAMES) {
                            mark = mark->Next();
                            if (!mark) break;
                            writePosition = mark->position;
                            for (int i = 0; i < DEBUG_MARK_FRAMES; i++) {
                                if (!macontext.Config->fullDecode) writePosition = recordingIndexMark->GetIFrameBefore(writePosition - 1);
                                else writePosition--;
                            }
                            writeOffset = -DEBUG_MARK_FRAMES;
                        }
                        else writeOffset++;
                    }
                }
            }
        }
    }
}
#endif


void cMarkAdStandalone::MarkadCut() {
    if (abortNow) return;
    if (!ptr_cDecoder) {
        dsyslog("cMarkAdStandalone::MarkadCut(): ptr_cDecoder not set");
        return;
    }
    LogSeparator(true);
    isyslog("start cut video based on marks");
    if (marks.Count() < 2) {
        isyslog("need at least 2 marks to cut Video");
        return; // we cannot do much without marks
    }
    dsyslog("cMarkAdStandalone::MarkadCut(): final marks are:");
    DebugMarks();     //  only for debugging

    // init encoder
    cEncoder *ptr_cEncoder = new cEncoder(&macontext);
    ALLOC(sizeof(*ptr_cEncoder), "ptr_cEncoder");

    int passMin = 0;
    int passMax = 0;
    if (macontext.Config->fullEncode) {  // to full endcode we need 2 pass full encoding
        passMin = 1;
        passMax = 2;
    }

    for (int pass = passMin; pass <= passMax; pass ++) {
        dsyslog("cMarkAdStandalone::MarkadCut(): start pass %d", pass);
        ptr_cDecoder->Reset();
        ptr_cDecoder->DecodeDir(directory);
        ptr_cEncoder->Reset(pass);

        // set start and end mark of first part
        cMark *startMark = marks.GetFirst();
        if ((startMark->type & 0x0F) != MT_START) {
            esyslog("got invalid start mark at (%i) type 0x%X", startMark->position, startMark->type);
            return;
        }
        int startPosition;
        if (macontext.Config->fullEncode) startPosition = recordingIndexMark->GetIFrameBefore(startMark->position - 1);  // go before mark position to preload decoder pipe
        else startPosition = recordingIndexMark->GetIFrameAfter(startMark->position);  // go after mark position to prevent last picture of ad
        if (startPosition < 0) startPosition = startMark->position;

        cMark *stopMark = startMark->Next();
        if ((stopMark->type & 0x0F) != MT_STOP) {
            esyslog("got invalid stop mark at (%i) type 0x%X", stopMark->position, stopMark->type);
            return;
        }

        // open output file
        ptr_cDecoder->SeekToFrame(&macontext, startPosition);  // seek to start posiition to get correct input video parameter
        if (!ptr_cEncoder->OpenFile(directory, ptr_cDecoder)) {
            esyslog("failed to open output file");
            FREE(sizeof(*ptr_cEncoder), "ptr_cEncoder");
            delete ptr_cEncoder;
            ptr_cEncoder = NULL;
            return;
        }

        bool nextFile = true;
        // process input file
        while(nextFile && ptr_cDecoder->DecodeDir(directory)) {
            while(ptr_cDecoder->GetNextPacket()) {
                int frameNumber = ptr_cDecoder->GetFrameNumber();
                if  (frameNumber < startPosition) {  // go to start frame
                    LogSeparator();
                    dsyslog("cMarkAdStandalone::MarkadCut(): decoding from frame (%d) for start mark (%d) to frame (%d) in pass %d", startPosition, startMark->position, stopMark->position, pass);
                    ptr_cDecoder->SeekToFrame(&macontext, startPosition);
                    frameNumber = ptr_cDecoder->GetFrameNumber();
                }
                if  (frameNumber > stopMark->position) {  // stop mark reached
                    if (stopMark->Next() && stopMark->Next()->Next()) {  // next mark pair
                        startMark = stopMark->Next();
                        if ((startMark->type & 0x0F) != MT_START) {
                            esyslog("got invalid start mark at (%i) type 0x%X", startMark->position, startMark->type);
                            return;
                        }

                        if (macontext.Config->fullEncode) startPosition = recordingIndexMark->GetIFrameBefore(startMark->position - 1);  // go before mark position to preload decoder pipe
                        else startPosition = recordingIndexMark->GetIFrameAfter(startMark->position);  // go after mark position to prevent last picture of ad
                        if (startPosition < 0) startPosition = startMark->position;

                        stopMark = startMark->Next();
                        if ((stopMark->type & 0x0F) != MT_STOP) {
                            esyslog("got invalid stop mark at (%i) type 0x%X", stopMark->position, stopMark->type);
                            return;
                        }
                    }
                    else {
                        nextFile = false;
                        break;
                    }
                    continue;
                }
                // read packet
                AVPacket *pkt = ptr_cDecoder->GetPacket();
                if (!pkt) {
                    esyslog("failed to get packet from input stream");
                    return;
                }
                // preload decoder pipe
                if ((macontext.Config->fullEncode) && (frameNumber < startMark->position)) {
                    ptr_cDecoder->DecodePacket(pkt);
                    continue;
                }
                // decode/encode/write packet
                if (!ptr_cEncoder->WritePacket(pkt, ptr_cDecoder)) {
                    dsyslog("cMarkAdStandalone::MarkadCut(): failed to write frame %d to output stream", frameNumber);  // no not abort, maybe next frame works
                }
                if (abortNow) {
                    ptr_cEncoder->CloseFile(ptr_cDecoder);  // ptr_cEncoder must be valid here because it is used above
                    if (ptr_cDecoder) {
                        FREE(sizeof(*ptr_cDecoder), "ptr_cDecoder");
                        delete ptr_cDecoder;
                        ptr_cDecoder = NULL;
                    }
                    FREE(sizeof(*ptr_cEncoder), "ptr_cEncoder");
                    delete ptr_cEncoder;
                    ptr_cEncoder = NULL;
                    return;
                }
            }
        }
        if (!ptr_cEncoder->CloseFile(ptr_cDecoder)) {
            dsyslog("failed to close output file");
            return;
        }
    }
    dsyslog("cMarkAdStandalone::MarkadCut(): end at frame %d", ptr_cDecoder->GetFrameNumber());
    FREE(sizeof(*ptr_cEncoder), "ptr_cEncoder");
    delete ptr_cEncoder;  // ptr_cEncoder must be valid here because it is used above
    ptr_cEncoder = NULL;
    framecnt4 = ptr_cDecoder->GetFrameNumber();
}


// 3nd pass
// move logo marks:
//     - if closing credits are detected after last logo stop mark
//     - if silence was detected before start mark or after/before end mark
//     - if black screen marks are direct before stop mark or direct after start mark
//
void cMarkAdStandalone::Process3ndPass() {
    if (!ptr_cDecoder) return;

    LogSeparator(true);
    isyslog("start 3nd pass (optimze logo marks)");
    LogSeparator(false);
    dsyslog("cMarkAdStandalone::Process3ndPass(): check last logo stop mark if closing credits follows");

    bool save = false;
// check last logo stop mark if closing credits follows
    if (ptr_cDecoder) {  // we use file position from 2ndPass call
        cMark *lastStop = marks.GetLast();
        if (lastStop && ((lastStop->type == MT_LOGOSTOP) ||  (lastStop->type == MT_HBORDERSTOP))) {
            dsyslog("cMarkAdStandalone::Process3ndPass(): search for closing credits");
            if (MoveLastStopAfterClosingCredits(lastStop)) {
                dsyslog("cMarkAdStandalone::Process3ndPass(): moved last logo stop mark after closing credit");
            }
            save = true;
            framecnt3 = ptr_cDecoder->GetFrameNumber() - framecnt2;
        }
    }

// check for advertising in frame with logo after logo start mark and before logo stop mark and check for introduction logo
    LogSeparator(true);
    dsyslog("cMarkAdStandalone::Process3ndPass(): check for advertising in frame with logo after logo start and before logo stop mark and check for introduction logo");

    ptr_cDecoder->Reset();
    ptr_cDecoder->DecodeDir(directory);

    cDetectLogoStopStart *ptr_cDetectLogoStopStart = new cDetectLogoStopStart(&macontext, ptr_cDecoder, recordingIndexMark, NULL);
    ALLOC(sizeof(*ptr_cDetectLogoStopStart), "ptr_cDetectLogoStopStart");

    cMark *markLogo = marks.GetFirst();
    while (markLogo) {
        if (markLogo->type == MT_LOGOSTART) {
            char *indexToHMSFStartMark = marks.IndexToHMSF(markLogo->position, &macontext);

            // check for introduction logo before logo mark position
            LogSeparator(false);
            int searchStartPosition = markLogo->position - (30 * macontext.Video.Info.framesPerSecond); // introduction logos are usually 10s, somettimes longer, changed from 12 to 30
            if (searchStartPosition < 0) searchStartPosition = 0;
            char *indexToHMSFSearchStart = marks.IndexToHMSF(searchStartPosition, &macontext);
            if (indexToHMSFStartMark && indexToHMSFSearchStart) dsyslog("cMarkAdStandalone::Process3ndPass(): search introduction logo from position (%d) at %s to logo start mark (%d) at %s", searchStartPosition, indexToHMSFSearchStart, markLogo->position, indexToHMSFStartMark);
            if (indexToHMSFSearchStart) {
                FREE(strlen(indexToHMSFSearchStart)+1, "indexToHMSF");
                free(indexToHMSFSearchStart);
            }
            int introductionStartPosition = -1;
            if (ptr_cDetectLogoStopStart->Detect(searchStartPosition, markLogo->position, false)) {
                introductionStartPosition = ptr_cDetectLogoStopStart->IntroductionLogo();
            }

            // check for advertising in frame with logo after logo start mark position
            LogSeparator(false);
            int searchEndPosition = markLogo->position + (35 * macontext.Video.Info.framesPerSecond); // advertising in frame are usually 30s
            char *indexToHMSFSearchEnd = marks.IndexToHMSF(searchEndPosition, &macontext);
            if (indexToHMSFStartMark && indexToHMSFSearchEnd) dsyslog("cMarkAdStandalone::Process3ndPass(): search advertising in frame with logo after logo start mark (%d) at %s to position (%d) at %s", markLogo->position, indexToHMSFStartMark, searchEndPosition, indexToHMSFSearchEnd);
            if (indexToHMSFStartMark) {
                FREE(strlen(indexToHMSFStartMark)+1, "indexToHMSF");
                free(indexToHMSFStartMark);
            }
            if (indexToHMSFSearchEnd) {
                FREE(strlen(indexToHMSFSearchEnd)+1, "indexToHMSF");
                free(indexToHMSFSearchEnd);
            }
            int adInFrameEndPosition = -1;
            if (ptr_cDetectLogoStopStart->Detect(markLogo->position, searchEndPosition, true)) {
                adInFrameEndPosition = ptr_cDetectLogoStopStart->AdInFrameWithLogo(true);
            }
            if (adInFrameEndPosition >= 0) {
                dsyslog("cMarkAdStandalone::Process3ndPass(): ad in frame between (%d) and (%d) found", markLogo->position, adInFrameEndPosition);
                if (evaluateLogoStopStartPair && (evaluateLogoStopStartPair->IncludesInfoLogo(markLogo->position, adInFrameEndPosition))) {
                    dsyslog("cMarkAdStandalone::Process3ndPass(): deleted info logo part in this range, this could not be a advertising in frame");
                    adInFrameEndPosition = -1;
                }
            }
            if (adInFrameEndPosition != -1) {  // if we found advertising in frame, use this
                adInFrameEndPosition = recordingIndexMark->GetIFrameAfter(adInFrameEndPosition + 1);  // we got last frame of ad, go to next iFrame for start mark
                markLogo = marks.Move(&macontext, markLogo, adInFrameEndPosition, "advertising in frame");
                save = true;
            }
            else {
                if (introductionStartPosition != -1) {
                    bool move = true;
                    // check blackscreen between introduction logo start and logo start, there should be no long blackscreen, short blackscreen are from retrospect
                    cMark *blackMarkStart = blackMarks.GetNext(introductionStartPosition, MT_NOBLACKSTART);
                    cMark *blackMarkStop = blackMarks.GetNext(introductionStartPosition, MT_NOBLACKSTOP);
                    if (blackMarkStart && blackMarkStop && (blackMarkStart->position <= markLogo->position) && (blackMarkStop->position <= markLogo->position)) {
                        int innerLength = 1000 * (blackMarkStart->position - blackMarkStop->position) / macontext.Video.Info.framesPerSecond;
                        dsyslog("cMarkAdStandalone::Process3ndPass(): found blackscreen start (%d) and stop (%d) between introduction logo (%d) and start mark (%d), length %dms", blackMarkStop->position, blackMarkStart->position, introductionStartPosition, markLogo->position, innerLength);
                        if (innerLength > 1000) move = false;  // only move if we found no long blackscreen between introduction logo and logo start
                    }
                    if (move) {
                        // check blackscreen before introduction logo
                        blackMarkStart = blackMarks.GetPrev(introductionStartPosition, MT_NOBLACKSTART);
                        blackMarkStop = blackMarks.GetPrev(introductionStartPosition, MT_NOBLACKSTOP);
                        if (blackMarkStart && blackMarkStop) {
                            int beforeLength = 1000 * (blackMarkStart->position - blackMarkStop->position)  / macontext.Video.Info.framesPerSecond;
                            int diff = 1000 * (introductionStartPosition - blackMarkStart->position) / macontext.Video.Info.framesPerSecond;
                            dsyslog("cMarkAdStandalone::Process3ndPass(): found blackscreen start (%d) and stop (%d) before introduction logo (%d), distance %dms, length %dms", blackMarkStop->position, blackMarkStart->position, introductionStartPosition, diff, beforeLength);
                            if (diff <= 3520) { // blackscreen beforeshould be near
                                dsyslog("cMarkAdStandalone::Process3ndPass(): found valid blackscreen at (%d), %dms before introduction logo", blackMarkStart->position, diff);
                                markLogo = marks.Move(&macontext, markLogo, blackMarkStart->position, "blackscreen before introduction logo");
                                move = false;  // move is done based on blackscreen position
                            }
                        }
                    }
                    if (move) markLogo = marks.Move(&macontext, markLogo, introductionStartPosition, "introduction logo");
                    save = true;
                }
            }
        }
        if ((markLogo->type == MT_LOGOSTOP) && (marks.GetNext(markLogo->position, MT_STOP, 0x0F))) { // do not test logo end mark, ad in frame with logo and closing credits without logo looks the same
            // check for advertising in frame with logo before logo stop mark position
            LogSeparator(false);
            int searchStartPosition = markLogo->position - (45 * macontext.Video.Info.framesPerSecond); // advertising in frame are usually 30s, changed from 35 to 45
                                                                                                        // somtimes there is a closing credit in frame with logo before
            char *indexToHMSFStopMark = marks.IndexToHMSF(markLogo->position, &macontext);
            char *indexToHMSFSearchPosition = marks.IndexToHMSF(searchStartPosition, &macontext);
            if (indexToHMSFStopMark && indexToHMSFSearchPosition) dsyslog("cMarkAdStandalone::Process3ndPass(): search advertising in frame with logo from frame (%d) at %s to logo stop mark (%d) at %s", searchStartPosition, indexToHMSFSearchPosition, markLogo->position, indexToHMSFStopMark);
            if (indexToHMSFStopMark) {
                FREE(strlen(indexToHMSFStopMark)+1, "indexToHMSF");
                free(indexToHMSFStopMark);
            }
            if (indexToHMSFSearchPosition) {
                FREE(strlen(indexToHMSFSearchPosition)+1, "indexToHMSF");
                free(indexToHMSFSearchPosition);
            }
            // short start/stop pair can result in overlapping checks
            if (ptr_cDecoder->GetFrameNumber() > searchStartPosition) {
                dsyslog("cMarkAdStandalone::Process3ndPass(): current framenumber (%d) greater than framenumber to seek (%d), restart decoder", ptr_cDecoder->GetFrameNumber(), searchStartPosition);
                ptr_cDecoder->Reset();
                ptr_cDecoder->DecodeDir(directory);
            }
            // detect frames
            if (ptr_cDetectLogoStopStart->Detect(searchStartPosition, markLogo->position, true)) {
                int newStopPosition = ptr_cDetectLogoStopStart->AdInFrameWithLogo(false);
                if (newStopPosition != -1) {
                    newStopPosition = recordingIndexMark->GetIFrameBefore(newStopPosition - 1);  // we got first frame of ad, go one iFrame back for stop mark
                    markLogo = marks.Move(&macontext, markLogo, newStopPosition, "advertising in frame");
                    save = true;
               }
            }
        }
        markLogo = markLogo->Next();
    }
    FREE(sizeof(*evaluateLogoStopStartPair), "evaluateLogoStopStartPair");
    delete evaluateLogoStopStartPair;
    FREE(sizeof(*ptr_cDetectLogoStopStart), "ptr_cDetectLogoStopStart");
    delete ptr_cDetectLogoStopStart;

// search for audio silence near logo marks
    LogSeparator(false);
    dsyslog("cMarkAdStandalone::Process3ndPass(): search for audio silence around logo marks");
    int silenceRange = 5;  // do not increase, otherwise we got stop marks behind separation images
    if (strcmp(macontext.Info.ChannelName, "DMAX")   == 0) silenceRange = 12; // logo color change at the begin
    if ((strcmp(macontext.Info.ChannelName, "TELE_5") == 0) ||
        (strcmp(macontext.Info.ChannelName, "Nickelodeon") == 0)) silenceRange =  7; // logo fade in/out

    ptr_cDecoder->Reset();
    ptr_cDecoder->DecodeDir(directory);

    char *indexToHMSF = NULL;
    cMark *mark = marks.GetFirst();
    while (mark) {
        if (indexToHMSF) {
           FREE(strlen(indexToHMSF)+1, "indexToHMSF");
           free(indexToHMSF);
        }
        indexToHMSF = marks.IndexToHMSF(mark->position, &macontext);

        if (mark->type == MT_LOGOSTART) {
            if (indexToHMSF) dsyslog("cMarkAdStandalone::Process3ndPass(): detect audio silence before logo mark at frame (%6i) type 0x%X at %s range %is", mark->position, mark->type, indexToHMSF, silenceRange);
            int seekPos =  mark->position - (silenceRange * macontext.Video.Info.framesPerSecond);
            if (seekPos < 0) seekPos = 0;
            if (!ptr_cDecoder->SeekToFrame(&macontext, seekPos)) {
                esyslog("could not seek to frame (%i)", mark->position);
                break;
            }
            framecnt3 += silenceRange * macontext.Video.Info.framesPerSecond;
            int beforeSilence = ptr_cDecoder->GetNextSilence(&macontext, mark->position, true, true);
            if ((beforeSilence >= 0) && (beforeSilence != mark->position)) {
                dsyslog("cMarkAdStandalone::Process3ndPass(): found audio silence before logo start at frame (%i)", beforeSilence);
                // search for blackscreen near silence to optimize mark positon
                cMark *blackMark = blackMarks.GetAround(1 * macontext.Video.Info.framesPerSecond, beforeSilence, MT_NOBLACKSTART);
                if (blackMark) mark = marks.Move(&macontext, mark, blackMark->position, "black screen near silence");
                else mark = marks.Move(&macontext, mark, beforeSilence, "silence");
                save = true;
                continue;
            }
            if (indexToHMSF) dsyslog("cMarkAdStandalone::Process3ndPass(): no audio silence before logo mark at frame (%6i) type 0x%X at %s found", mark->position, mark->type, indexToHMSF);

        }
        if (mark->type == MT_LOGOSTOP) {
            // search before stop mark
            if (indexToHMSF) dsyslog("cMarkAdStandalone::Process3ndPass(): detect audio silence before logo stop mark at frame (%6i) type 0x%X at %s range %i", mark->position, mark->type, indexToHMSF, silenceRange);
            int seekPos =  mark->position - (silenceRange * macontext.Video.Info.framesPerSecond);
            if (seekPos < ptr_cDecoder->GetFrameNumber()) seekPos = ptr_cDecoder->GetFrameNumber();  // will retun -1 before first frame read
            if (seekPos < 0) seekPos = 0;
            if (!ptr_cDecoder->SeekToFrame(&macontext, seekPos)) {
                esyslog("could not seek to frame (%i)", mark->position);
                break;
            }
            int beforeSilence = ptr_cDecoder->GetNextSilence(&macontext, mark->position, true, false);
            if (beforeSilence >= 0) dsyslog("cMarkAdStandalone::Process3ndPass(): found audio silence before logo stop mark (%i) at frame (%i)", mark->position, beforeSilence);

            // search after stop mark
            if (indexToHMSF) dsyslog("cMarkAdStandalone::Process3ndPass(): detect audio silence after logo stop mark at frame (%6i) type 0x%X at %s range %i", mark->position, mark->type, indexToHMSF, silenceRange);
            if (!ptr_cDecoder->SeekToFrame(&macontext, mark->position)) {
                esyslog("could not seek to frame (%i)", mark->position);
                break;
            }
            int stopFrame =  mark->position + ((silenceRange - 1) * macontext.Video.Info.framesPerSecond);  // reduce detection range after logo stop to avoid to get stop mark after separation image
            int afterSilence = ptr_cDecoder->GetNextSilence(&macontext, stopFrame, false, false);
            if (afterSilence >= 0) dsyslog("cMarkAdStandalone::Process3ndPass(): found audio silence after logo stop mark (%i) at iFrame (%i)", mark->position, afterSilence);
            framecnt3 += 2 * (silenceRange - 1) * macontext.Video.Info.framesPerSecond;
            bool before = false;

            // use before silence only if we found no after silence
            if (afterSilence < 0) {
                afterSilence = beforeSilence;
                before = true;
            }

            if ((afterSilence >= 0) && (afterSilence != mark->position)) {
                if (indexToHMSF) dsyslog("cMarkAdStandalone::Process3ndPass(): found audio silence for mark at frame (%6i) type 0x%X at %s range %i", mark->position, mark->type, indexToHMSF, silenceRange);
                dsyslog("cMarkAdStandalone::Process3ndPass(): use audio silence %s logo stop at iFrame (%i)", (before) ? "before" : "after", afterSilence);
                // search for blackscreen near silence to optimize mark positon
                cMark *blackMark = blackMarks.GetAround(1 * macontext.Video.Info.framesPerSecond, afterSilence, MT_NOBLACKSTART);
                if (blackMark) mark = marks.Move(&macontext, mark, blackMark->position - 1, "black screen near silence"); // MT_NOBLACKSTART is first frame after black screen
                else mark = marks.Move(&macontext, mark, afterSilence, "silence");
                save = true;
                continue;
            }
        }
        mark=mark->Next();
    }
    if (indexToHMSF) {  // cleanup after loop
        FREE(strlen(indexToHMSF)+1, "indexToHMSF");
        free(indexToHMSF);
    }

// try blacksceen mark
    LogSeparator(false);
    dsyslog("cMarkAdStandalone::Process3ndPass(): start search for blackscreen near logo marks");
    int blackscreenRange = 4270;
    // logo fade in/out
    if ((strcmp(macontext.Info.ChannelName, "TELE_5")         == 0) ||
        (strcmp(macontext.Info.ChannelName, "Disney_Channel") == 0) ||
        (strcmp(macontext.Info.ChannelName, "Nickelodeon")    == 0)) blackscreenRange = 5500;
    mark = marks.GetFirst();
    while (mark) {
        // logo start mark, use blackscreen before and after mark
        if (mark->type == MT_LOGOSTART) {
            cMark *blackMark = blackMarks.GetAround(blackscreenRange * macontext.Video.Info.framesPerSecond / 1000, mark->position, MT_NOBLACKSTART); // blacksceen belongs to previous broadcast, use first frame after
            if (blackMark) {
                int distance = mark->position - blackMark->position;
                int distance_ms = 1000 * distance / macontext.Video.Info.framesPerSecond;
                if (distance > 0)  { // blackscreen is before logo start mark
                    dsyslog("cMarkAdStandalone::Process3ndPass(): blackscreen (%d) distance (%d frames) %dms (expect >0 and <=%dms) before logo start mark (%d), move mark", blackMark->position, distance, distance_ms, blackscreenRange, mark->position);
                    mark = marks.Move(&macontext, mark, blackMark->position, "black screen");
                    save = true;
                    continue;
                }
                else dsyslog("cMarkAdStandalone::Process3ndPass(): blackscreen (%d) distance (%d) %ds (expect >0 and <=%ds) before (-after) logo start mark (%d), keep mark", blackMark->position, distance, distance_ms, blackscreenRange, mark->position);
            }
            else dsyslog("cMarkAdStandalone::Process3ndPass(): no black screen mark found before logo start mark (%d)", mark->position);
        }
        // logo stop mark or blackscreen start (=stop mark, this can only be a end mark, move mark to end of black screen range)
        // use black screen mark only after mark
        if ((mark->type == MT_LOGOSTOP) || (mark->type == MT_NOBLACKSTOP)) {
            cMark *blackMark = blackMarks.GetNext(mark->position, MT_NOBLACKSTART);
            if (blackMark) {
                int diff_ms = 1000 * (blackMark->position - mark->position) / macontext.Video.Info.framesPerSecond;
                dsyslog("cMarkAdStandalone::Process3ndPass(): blackscreen (%d) %dms (expect <=%ds) after logo stop mark (%d) found", blackMark->position, diff_ms, blackscreenRange, mark->position);
                if (diff_ms <= blackscreenRange) {
                    int newPos;
                    if (!macontext.Config->fullDecode) {
                        newPos =  recordingIndexMark->GetIFrameBefore(blackMark->position); // MT_NOBLACKSSTART with "only iFrame decoding" is the first frame afer blackscreen, get last frame of blackscreen, blacksceen at stop mark belongs to broasdact
                    }
                    else newPos = blackMark->position - 1; // MT_NOBLACKSTART is first frame after blackscreen, go one back

                    if (newPos == mark->position) { // found blackscreen at same position
                        mark = mark->Next();
                        continue;
                    }
                    mark = marks.Move(&macontext, mark, newPos, "black screen");
                    save = true;
                    continue;
                }
            }
            else dsyslog("cMarkAdStandalone::Process3ndPass(): no black screen mark found after logo stop mark (%d)", mark->position);
        }
        mark = mark->Next();
    }

    if (save) marks.Save(directory, &macontext, false);
    return;
}


void cMarkAdStandalone::Process2ndPass() {
    if (abortNow) return;
    if (duplicate) return;
    if (!ptr_cDecoder) return;
    if (!length) return;
    if (!startTime) return;
    if (time(NULL) < (startTime+(time_t) length)) return;

    LogSeparator(true);
    isyslog("start 2nd pass (detect overlaps)");

    if (!macontext.Video.Info.framesPerSecond) {
        isyslog("WARNING: assuming fps of 25");
        macontext.Video.Info.framesPerSecond = 25;
    }

    cMark *p1 = NULL,*p2 = NULL;

    if (ptr_cDecoder) {
        ptr_cDecoder->Reset();
        ptr_cDecoder->DecodeDir(directory);
    }

    if (marks.Count() >= 4) {
        p1 = marks.GetFirst();
        if (!p1) return;

        p1 = p1->Next();
        if (p1) p2 = p1->Next();

        while ((p1) && (p2)) {
            if (ptr_cDecoder) {
                dsyslog("cMarkAdStandalone::Process2ndPass(): ->->->->-> check overlap before stop frame (%d) and after start frame (%d)", p1->position, p2->position);
                // init overlap detection object
                cMarkAdOverlap *overlap = new cMarkAdOverlap(&macontext);
                ALLOC(sizeof(*overlap), "overlap");
                // detect overlap before stop and after start
                if (!ProcessMark2ndPass(overlap, &p1, &p2)) {
                    dsyslog("cMarkAdStandalone::Process2ndPass(): no overlap found for marks before frames (%d) and after (%d)", p1->position, p2->position);
                }
                // free overlap detection object
                FREE(sizeof(*overlap), "overlap");
                delete overlap;
            }
            p1 = p2->Next();
            if (p1) {
                p2 = p1->Next();
            }
            else {
                p2 = NULL;
            }
        }
    }
    framecnt2 = ptr_cDecoder->GetFrameNumber();
    dsyslog("end 2ndPass");
    return;
}


void cMarkAdStandalone::Reset() {
    iFrameBefore = -1;
    iFrameCurrent = -1;
    frameCurrent = -1;
    gotendmark = false;
    chkSTART = chkSTOP = INT_MAX;
    macontext.Video.Info.AspectRatio.den = 0;
    macontext.Video.Info.AspectRatio.num = 0;
    memset(macontext.Audio.Info.Channels, 0, sizeof(macontext.Audio.Info.Channels));

    if (video) video->Clear(false);
    if (audio) audio->Clear();
    return;
}


bool cMarkAdStandalone::ProcessFrame(cDecoder *ptr_cDecoder) {
    if (!ptr_cDecoder) return false;
    if (!video) {
        esyslog("cMarkAdStandalone::ProcessFrame() video not initialized");
        return false;
    }

    if ((macontext.Config->logoExtraction != -1) && (ptr_cDecoder->GetIFrameCount() >= 512)) {    // extract logo
        isyslog("finished logo extraction, please check /tmp for pgm files");
        abortNow=true;
    }
    frameCurrent = ptr_cDecoder->GetFrameNumber();
    if (ptr_cDecoder->IsVideoIFrame()) {
        iFrameBefore = iFrameCurrent;
        iFrameCurrent = frameCurrent;
    }
    if (ptr_cDecoder->GetFrameInfo(&macontext, macontext.Config->fullDecode)) {
        if (ptr_cDecoder->IsVideoPacket()) {
            if ((ptr_cDecoder->GetFileNumber() == 1) &&  // found some Finnish H.264 interlaced recordings who changed real bite rate in second TS file header
                                                         // frame rate can not change, ignore this and keep frame rate from first TS file
                 ptr_cDecoder->IsInterlacedVideo() && !macontext.Video.Info.interlaced && (macontext.Info.vPidType==MARKAD_PIDTYPE_VIDEO_H264) &&
                (ptr_cDecoder->GetVideoAvgFrameRate() == 25) && (ptr_cDecoder->GetVideoRealFrameRate() == 50)) {
                dsyslog("cMarkAdStandalone::ProcessFrame(): change internal frame rate to handle H.264 interlaced video");
                macontext.Video.Info.framesPerSecond *= 2;
                macontext.Video.Info.interlaced = true;
                CalculateCheckPositions(macontext.Info.tStart * macontext.Video.Info.framesPerSecond);
            }
            if ((iStart < 0) && (frameCurrent > -iStart)) iStart = frameCurrent;
            if ((iStop < 0) && (frameCurrent > -iStop)) {
                iStop = frameCurrent;
                iStopinBroadCast = inBroadCast;
            }
            if ((iStopA < 0) && (frameCurrent > -iStopA)) {
                iStopA = frameCurrent;
            }

            if (!macontext.Video.Data.valid) {
                isyslog("failed to get video data of frame (%d)", ptr_cDecoder->GetFrameNumber());
                return false;
            }

            if (!restartLogoDetectionDone && (frameCurrent > (iStopA-macontext.Video.Info.framesPerSecond * 2 * MAXRANGE))) {
                dsyslog("cMarkAdStandalone::ProcessFrame(): enter end part at frame (%d)", frameCurrent);
                restartLogoDetectionDone = true;
                if ((macontext.Video.Options.ignoreBlackScreenDetection) || (macontext.Video.Options.ignoreLogoDetection)) {
                    isyslog("restart logo and black screen detection at frame (%d)", ptr_cDecoder->GetFrameNumber());
                    bDecodeVideo = true;
                    macontext.Video.Options.ignoreBlackScreenDetection = false;   // use black sceen setection only to find end mark
                    if (macontext.Video.Options.ignoreLogoDetection == true) {
                        if (macontext.Video.Info.hasBorder) { // we do not need logos, we have hborder
                            dsyslog("cMarkAdStandalone::ProcessFrame(): we do not need to look for logos, we have a broadcast with border");
                        }
                        else {
                            macontext.Video.Options.ignoreLogoDetection = false;
                            if (video) video->Clear(true, inBroadCast);    // reset logo detector status
                        }
                    }
                }
            }

#ifdef DEBUG_LOGO_DETECT_FRAME_CORNER
            if ((iFrameCurrent > (DEBUG_LOGO_DETECT_FRAME_CORNER - 200)) && (iFrameCurrent < (DEBUG_LOGO_DETECT_FRAME_CORNER + 200))) {
//                dsyslog("save frame (%i) to /tmp", iFrameCurrent);
                SaveFrame(iFrameCurrent);
            }
#endif

            if (!bDecodeVideo) macontext.Video.Data.valid = false; // make video picture invalid, we do not need them
            sMarkAdMarks *vmarks = video->Process(iFrameBefore, iFrameCurrent, frameCurrent);
            if (vmarks) {
                for (int i = 0; i < vmarks->Count; i++) {
                    AddMark(&vmarks->Number[i]);
                }
            }

            if (iStart > 0) {
                if ((inBroadCast) && (frameCurrent > chkSTART)) CheckStart();
            }
            if ((iStop > 0) && (iStopA > 0)) {
                if (frameCurrent > chkSTOP) {
                    if (iStart != 0) {
                        dsyslog("still no chkStart called, doing it now");
                        CheckStart();
                    }
                    CheckStop();
                    return false;
                }
            }
        }
        if (ptr_cDecoder->IsVideoIFrame()) {  // check audio channels on next iFrame because audio changes are not at iFrame positions
            sMarkAdMark *amark = audio->Process();  // class audio will take frame number of channel change from macontext->Audio.Info.frameChannelChange
            if (amark) AddMark(amark);
        }
    }
    return true;
}


void cMarkAdStandalone::ProcessFiles() {
    if (abortNow) return;

    if (macontext.Config->backupMarks) marks.Backup(directory);

    LogSeparator(true);
    dsyslog("cMarkAdStandalone::ProcessFiles(): start processing files");
    ptr_cDecoder = new cDecoder(macontext.Config->threads, recordingIndexMark);
    ALLOC(sizeof(*ptr_cDecoder), "ptr_cDecoder");
    CheckIndexGrowing();
    while(ptr_cDecoder && ptr_cDecoder->DecodeDir(directory)) {
        if (abortNow) {
            if (ptr_cDecoder) {
                FREE(sizeof(*ptr_cDecoder), "ptr_cDecoder");
                delete ptr_cDecoder;
                ptr_cDecoder = NULL;
            }
            break;
        }
        if(ptr_cDecoder->GetFrameNumber() < 0) {
            macontext.Info.vPidType = ptr_cDecoder->GetVideoType();
            if (macontext.Info.vPidType == 0) {
                dsyslog("cMarkAdStandalone::ProcessFiles(): video type not set");
                return;
            }
            macontext.Video.Info.height = ptr_cDecoder->GetVideoHeight();
            isyslog("video hight: %i", macontext.Video.Info.height);

            macontext.Video.Info.width = ptr_cDecoder->GetVideoWidth();
            isyslog("video width: %i", macontext.Video.Info.width);

            macontext.Video.Info.framesPerSecond = ptr_cDecoder->GetVideoAvgFrameRate();
            isyslog("average frame rate %i frames per second", static_cast<int> (macontext.Video.Info.framesPerSecond));
            isyslog("real frame rate    %i frames per second", ptr_cDecoder->GetVideoRealFrameRate());

            CalculateCheckPositions(macontext.Info.tStart * macontext.Video.Info.framesPerSecond);
        }
        while(ptr_cDecoder && ptr_cDecoder->GetNextPacket()) {
            if (abortNow) {
                if (ptr_cDecoder) {
                    FREE(sizeof(*ptr_cDecoder), "ptr_cDecoder");
                    delete ptr_cDecoder;
                    ptr_cDecoder = NULL;
                }
                break;
            }
            // write an early start mark for running recordings
            if (macontext.Info.isRunningRecording && !macontext.Info.isStartMarkSaved && (ptr_cDecoder->GetFrameNumber() >= (macontext.Info.tStart * macontext.Video.Info.framesPerSecond))) {
                dsyslog("cMarkAdStandalone::ProcessFiles(): recording is aktive, read frame (%d), now save dummy start mark at pre timer position %ds", ptr_cDecoder->GetFrameNumber(), macontext.Info.tStart);
                cMarks marksTMP;
                marksTMP.Add(MT_ASSUMEDSTART, ptr_cDecoder->GetFrameNumber(), "timer start", true);
                marksTMP.Save(macontext.Config->recDir, &macontext, true);
                macontext.Info.isStartMarkSaved = true;
            }

            if (!cMarkAdStandalone::ProcessFrame(ptr_cDecoder)) break;
            CheckIndexGrowing();
        }
    }

    if (!abortNow) {
        if (iStart !=0 ) {  // iStart will be 0 if iStart was called
            dsyslog("cMarkAdStandalone::ProcessFiles(): recording ends unexpected before chkSTART (%d) at frame %d", chkSTART, frameCurrent);
            isyslog("got end of recording before recording length from info file reached");
            CheckStart();
        }
        if (iStopA > 0) {
            if (iStop <= 0) {  // unexpected end of recording reached
                iStop = frameCurrent;
                iStopinBroadCast = true;
                dsyslog("cMarkAdStandalone::ProcessFiles(): recording ends unexpected before chkSTOP (%d) at frame %d", chkSTOP, frameCurrent);
                isyslog("got end of recording before recording length from info file reached");
            }
            CheckStop();
        }
        CheckMarks();
        if ((inBroadCast) && (!gotendmark) && (frameCurrent)) {
            sMarkAdMark tempmark;
            tempmark.type = MT_RECORDINGSTOP;
            tempmark.position = iFrameCurrent;
            AddMark(&tempmark);
        }
        if (marks.Save(directory, &macontext, false)) {
            if (length && startTime)
                    if (macontext.Config->saveInfo) SaveInfo();

        }
    }
    dsyslog("cMarkAdStandalone::ProcessFiles(): end processing files");
}


bool cMarkAdStandalone::SetFileUID(char *file) {
    if (!file) return false;
    struct stat statbuf;
    if (!stat(directory, &statbuf)) {
        if (chown(file, statbuf.st_uid, statbuf.st_gid) == -1) return false;
    }
    return true;
}


bool cMarkAdStandalone::SaveInfo() {
    isyslog("writing info file");
    char *src, *dst;
    if (isREEL) {
        if (asprintf(&src, "%s/info.txt", directory) == -1) return false;
    }
    else {
        if (asprintf(&src, "%s/info", directory) == -1) return false;
    }
    ALLOC(strlen(src)+1, "src");

    if (asprintf(&dst, "%s/info.bak", directory) == -1) {
        free(src);
        return false;
    }
    ALLOC(strlen(dst)+1, "src");

    FILE *r,*w;
    r = fopen(src, "r");
    if (!r) {
        free(src);
        free(dst);
        return false;
    }

    w=fopen(dst, "w+");
    if (!w) {
        fclose(r);
        free(src);
        free(dst);
        return false;
    }

    char *line = NULL;
    char *lline = NULL;
    size_t len = 0;

    char lang[4] = "";

    int component_type_add = 0;
    if (macontext.Video.Info.height > 576) component_type_add = 8;

    int stream_content = 0;
    if (macontext.Info.vPidType == MARKAD_PIDTYPE_VIDEO_H262) stream_content = 1;
    if (macontext.Info.vPidType == MARKAD_PIDTYPE_VIDEO_H264) stream_content = 5;

    int component_type_43;
    int component_type_169;
    if ((macontext.Video.Info.framesPerSecond == 25) || (macontext.Video.Info.framesPerSecond == 50)) {
        component_type_43 = 1;
        component_type_169 = 3;
    }
    else {
        component_type_43 = 5;
        component_type_169 = 7;
    }

    bool err = false;
    for (int i = 0; i < MAXSTREAMS; i++) {
        dsyslog("stream %i has %i channels", i, macontext.Info.Channels[i]);
    }
    unsigned int stream_index = 0;
    if (ptr_cDecoder) stream_index++;
    while (getline(&line, &len, r) != -1) {
        dsyslog("info file line: %s", line);
        if (line[0] == 'X') {
            int stream = 0;
            unsigned int type = 0;
            char descr[256] = "";

            int result=sscanf(line, "%*c %3i %3X %3c %250c", &stream, &type, (char *) &lang, (char *) &descr);
            if ((result != 0) && (result != EOF)) {
                switch (stream) {
                    case 1:
                    case 5:
                        if (stream == stream_content) {
                            if ((macontext.Info.AspectRatio.num == 4) && (macontext.Info.AspectRatio.den == 3)) {
                                if (fprintf(w, "X %i %02i %s 4:3\n", stream_content, component_type_43 + component_type_add, lang) <= 0) err = true;
                                macontext.Info.AspectRatio.num = 0;
                                macontext.Info.AspectRatio.den = 0;
                            }
                            else if ((macontext.Info.AspectRatio.num == 16) && (macontext.Info.AspectRatio.den == 9)) {
                                if (fprintf(w, "X %i %02X %s 16:9\n", stream_content, component_type_169 + component_type_add, lang) <= 0) err = true;
                                macontext.Info.AspectRatio.num = 0;
                                macontext.Info.AspectRatio.den = 0;
                            }
                            else {
                                if (fprintf(w, "%s",line) <=0 ) err = true;
                            }
                        }
                        else {
                            if (fprintf(w, "%s", line) <= 0) err = true;
                        }
                        break;
                    case 2:
                        if (type == 5) {
                            if (macontext.Info.Channels[stream_index] == 6) {
                                if (fprintf(w,"X 2 05 %s Dolby Digital 5.1\n", lang) <=0 ) err = true;
                                macontext.Info.Channels[stream_index] = 0;
                            }
                            else if (macontext.Info.Channels[stream_index] == 2) {
                                if (fprintf(w, "X 2 05 %s Dolby Digital 2.0\n", lang) <=0 ) err = true;
                                macontext.Info.Channels[stream_index] = 0;
                            }
                            else {
                                if (fprintf(w, "%s", line) <=0 ) err = true;
                            }
                        }
                        else {
                            if (fprintf(w, "%s", line) <=0 ) err = true;
                        }
                        break;
                    case 4:
                        if (type == 0x2C) {
                            if (fprintf(w, "%s", line) <=0 ) err = true;
                            macontext.Info.Channels[stream_index] = 0;
                            stream_index++;
                        }
                        break;
                    default:
                        if (fprintf(w, "%s", line) <=0 ) err = true;
                        break;
                }
            }
        }
        else {
            if (line[0] != '@') {
                if (fprintf(w, "%s", line) <=0 ) err = true;
            }
            else {
                if (lline) {
                    free(lline);
                    err = true;
                    esyslog("multiple @lines in info file, please report this!");
                }
                lline=strdup(line);
                ALLOC(strlen(lline)+1, "lline");
            }
        }
        if (err) break;
    }
    if (line) free(line);
    line=lline;

    if (lang[0] == 0) strcpy(lang, "und");

    if (stream_content) {
        if ((macontext.Info.AspectRatio.num == 4) && (macontext.Info.AspectRatio.den == 3) && (!err)) {
            if (fprintf(w, "X %i %02i %s 4:3\n", stream_content, component_type_43 + component_type_add, lang) <= 0 ) err = true;
        }
        if ((macontext.Info.AspectRatio.num == 16) && (macontext.Info.AspectRatio.den == 9) && (!err)) {
            if (fprintf(w, "X %i %02i %s 16:9\n", stream_content, component_type_169 + component_type_add, lang) <= 0) err = true;
        }
    }
    for (short int stream = 0; stream < MAXSTREAMS; stream++) {
        if (macontext.Info.Channels[stream] == 0) continue;
        if ((macontext.Info.Channels[stream] == 2) && (!err)) {
            if (fprintf(w, "X 2 05 %s Dolby Digital 2.0\n", lang) <= 0) err = true;
        }
        if ((macontext.Info.Channels[stream] == 6) && (!err)) {
            if (fprintf(w, "X 2 05 %s Dolby Digital 5.1\n", lang) <= 0) err = true;
       }
    }
    if (line) {
        if (fprintf(w, "%s", line) <=0 ) err = true;
        free(line);
    }
    fclose(w);
    struct stat statbuf_r;
    if (fstat(fileno(r), &statbuf_r) == -1) err = true;

    fclose(r);
    if (err) {
        unlink(dst);
    }
    else {
        if (rename(dst, src) == -1) {
            err = true;
        }
        else {
            // preserve timestamps from old file
            struct utimbuf oldtimes;
            oldtimes.actime = statbuf_r.st_atime;
            oldtimes.modtime = statbuf_r.st_mtime;
            if (utime(src, &oldtimes)) {};
            SetFileUID(src);
        }
    }

    free(src);
    free(dst);
    return (err==false);
}


bool cMarkAdStandalone::IsVPSTimer() {
    if (!directory) return false;
    bool timerVPS = false;

    char *fpath = NULL;
    if (asprintf(&fpath, "%s/%s", directory, "markad.vps") == -1) return false;
    ALLOC(strlen(fpath)+1, "fpath");

    FILE *mf;
    mf = fopen(fpath, "r");
    if (!mf) {
        dsyslog("cMarkAdStandalone::isVPSTimer(): markad.vps not found");
        FREE(strlen(fpath)+1, "fpath");
        free(fpath);
        return false;
    }
    FREE(strlen(fpath)+1, "fpath");
    free(fpath);

    char *line = NULL;
    size_t length;
    char vpsTimer[12] = "";
    while (getline(&line, &length, mf) != -1) {
        sscanf(line, "%12s", (char *) &vpsTimer);
        if (strcmp(vpsTimer, "VPSTIMER=YES") == 0) {
            timerVPS = true;
            break;
        }
    }
    if (line) free(line);
    fclose(mf);
    return timerVPS;
}


time_t cMarkAdStandalone::GetRecordingStart(time_t start, int fd) {
    // get recording start from atime of directory (if the volume is mounted with noatime)
    struct mntent *ent;
    struct stat statbuf;
    FILE *mounts = setmntent(_PATH_MOUNTED, "r");
    int mlen;
    int oldmlen = 0;
    bool useatime = false;
    while ((ent = getmntent(mounts)) != NULL) {
        if (strstr(directory, ent->mnt_dir)) {
            mlen = strlen(ent->mnt_dir);
            if (mlen > oldmlen) {
                if (strstr(ent->mnt_opts, "noatime")) {
                    useatime = true;
                }
                else {
                    useatime = false;
                }
            }
            oldmlen = mlen;
        }
    }
    endmntent(mounts);

    if (useatime) dsyslog("cMarkAdStandalone::GetRecordingStart(): mount option noatime is set, use atime from directory %s to get creation time", directory);
    else dsyslog("cMarkAdStandalone::GetRecordingStart(): mount option noatime is not set");

    if ((useatime) && (stat(directory, &statbuf) != -1)) {
        if (fabs(difftime(start,statbuf.st_atime)) < 60 * 60 * 12) {  // do not beleave recordings > 12h
            dsyslog("cMarkAdStandalone::GetRecordingStart(): got recording start from directory creation time");
            return statbuf.st_atime;
        }
        dsyslog("cMarkAdStandalone::GetRecordingStart(): got no valid directory creation time, maybe recording was copied %s", strtok(ctime(&statbuf.st_atime), "\n"));
        dsyslog("cMarkAdStandalone::GetRecordingStart(): broadcast start time from vdr info file                          %s", strtok(ctime(&start), "\n"));
    }

    // try to get from mtime
    // (and hope info.vdr has not changed after the start of the recording)
    if (fstat(fd,&statbuf) != -1) {
        if (fabs(difftime(start, statbuf.st_mtime)) < 7200) {
            dsyslog("cMarkAdStandalone::GetRecordingStart(): getting recording start from VDR info file modification time     %s", strtok(ctime(&statbuf.st_mtime), "\n"));
            return (time_t) statbuf.st_mtime;
        }
    }

    // fallback to the directory name (time part)
    const char *timestr = strrchr(directory, '/');
    if (timestr) {
        timestr++;
        if (isdigit(*timestr)) {
            time_t now = time(NULL);
            struct tm tm_r;
            struct tm t = *localtime_r(&now, &tm_r); // init timezone
            if (sscanf(timestr, "%4d-%02d-%02d.%02d%*c%02d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, & t.tm_min)==5) {
                t.tm_year -= 1900;
                t.tm_mon--;
                t.tm_sec = 0;
                t.tm_isdst = -1;
                isyslog("getting recording start from directory (can be wrong!)");
                return mktime(&t);
            }
        }
    }
    return (time_t) 0;
}


bool cMarkAdStandalone::CheckLogo() {
    if (!macontext.Config) return false;
    if (!macontext.Config->logoDirectory) return false;
    if (!macontext.Info.ChannelName) return false;
    int len=strlen(macontext.Info.ChannelName);
    if (!len) return false;

    dsyslog("cMarkAdStandalone::CheckLogo(): using logo directory %s", macontext.Config->logoDirectory);
    dsyslog("cMarkAdStandalone::CheckLogo(): searching logo for %s", macontext.Info.ChannelName);
    DIR *dir = opendir(macontext.Config->logoDirectory);
    if (!dir) return false;

    struct dirent *dirent = NULL;
    while ((dirent = readdir(dir))) {
        if (!strncmp(dirent->d_name, macontext.Info.ChannelName, len)) {
            closedir(dir);
            return true;
        }
    }
    closedir(dir);

    if (macontext.Config->autoLogo > 0) {
        isyslog("no logo found in logo directory, trying to find logo in recording directory");
        DIR *recDIR = opendir(macontext.Config->recDir);
        if (recDIR) {
            struct dirent *direntRec = NULL;
            while ((direntRec = readdir(recDIR))) {
                if (!strncmp(direntRec->d_name, macontext.Info.ChannelName, len)) {
                    closedir(recDIR);
                    isyslog("logo found in recording directory");
                    return true;
                }
            }
            closedir(recDIR);
        }
        isyslog("no logo found in recording directory, trying to extract logo from recording");
        ptr_cExtractLogo = new cExtractLogo(&macontext, macontext.Info.AspectRatio, recordingIndexMark);
        ALLOC(sizeof(*ptr_cExtractLogo), "ptr_cExtractLogo");
        int startPos =  macontext.Info.tStart * 25;  // search logo from assumed start, we do not know the frame rate at this point, so we use 25
        if (startPos < 0) startPos = 0;  // consider late start of recording
        int endpos = ptr_cExtractLogo->SearchLogo(&macontext, startPos);
        for (int retry = 2; retry <= 6; retry++) {  // do not reduce, we will not get some logos
            startPos += 5 * 60 * 25; // next try 5 min later
            if (endpos > 0) {
                isyslog("no logo found in recording, retry in %ind recording part", retry);
                endpos = ptr_cExtractLogo->SearchLogo(&macontext, startPos);
            }
            else break;
        }
        if (ptr_cExtractLogo) {
            FREE(sizeof(*ptr_cExtractLogo), "ptr_cExtractLogo");
            delete ptr_cExtractLogo;
            ptr_cExtractLogo = NULL;
        }
        if (endpos == 0) {
            dsyslog("cMarkAdStandalone::CheckLogo(): found logo in recording");
            return true;
        }
        else {
            dsyslog("cMarkAdStandalone::CheckLogo(): logo search failed");
            return false;
        }
    }
    return false;
}


bool cMarkAdStandalone::LoadInfo() {
    char *buf;
    if (asprintf(&buf, "%s/info", directory) == -1) return false;
    ALLOC(strlen(buf)+1, "buf");

    if (macontext.Config->before) {
        macontext.Info.isRunningRecording = true;
        dsyslog("parameter before is set, markad is called with a running recording");
    }

    FILE *f;
    f = fopen(buf, "r");
    FREE(strlen(buf)+1, "buf");
    free(buf);
    buf = NULL;
    if (!f) {
        // second try for reel vdr
        if (asprintf(&buf, "%s/info.txt", directory) == -1) return false;
        ALLOC(strlen(buf)+1, "buf");
        f = fopen(buf,"r");
        FREE(strlen(buf)+1, "buf");
        free(buf);
        if (!f) return false;
        isREEL = true;
    }

    char *line = NULL;
    size_t linelen;
    while (getline(&line, &linelen, f) != -1) {
        if (line[0] == 'C') {
            char channelname[256] = "";
            int result = sscanf(line, "%*c %*80s %250c", (char *) &channelname);
            if (result == 1) {
                macontext.Info.ChannelName = strdup(channelname);
                ALLOC(strlen(macontext.Info.ChannelName)+1, "macontext.Info.ChannelName");
                char *lf = strchr(macontext.Info.ChannelName, 10);
                if (lf) {
                   *lf = 0;
                    char *tmpName = strdup(macontext.Info.ChannelName);
                    ALLOC(strlen(tmpName)+1, "macontext.Info.ChannelName");
                    *lf = 10;
                    FREE(strlen(macontext.Info.ChannelName)+1, "macontext.Info.ChannelName");
                    free(macontext.Info.ChannelName);
                    macontext.Info.ChannelName = tmpName;
                }
                char *cr = strchr(macontext.Info.ChannelName, 13);
                if (cr) {
                    *cr = 0;
                    char *tmpName = strdup(macontext.Info.ChannelName);
                    ALLOC(strlen(tmpName)+1, "macontext.Info.ChannelName");
                    *lf = 13;
                    FREE(strlen(macontext.Info.ChannelName)+1, "macontext.Info.ChannelName");
                    free(macontext.Info.ChannelName);
                    macontext.Info.ChannelName = tmpName;
                }
                for (int i = 0; i < static_cast<int> (strlen(macontext.Info.ChannelName)); i++) {
                    if (macontext.Info.ChannelName[i] == ' ') macontext.Info.ChannelName[i] = '_';
                    if (macontext.Info.ChannelName[i] == '.') macontext.Info.ChannelName[i] = '_';
                    if (macontext.Info.ChannelName[i] == '/') macontext.Info.ChannelName[i] = '_';
                }
                if ((strcmp(macontext.Info.ChannelName, "SAT_1") == 0) || (strcmp(macontext.Info.ChannelName, "SAT_1_HD")) == 0) {
                    dsyslog("cMarkAdStandalone::LoadInfo(): channel %s has a rotating logo", macontext.Info.ChannelName);
                    macontext.Video.Logo.isRotating = true;
                }
            }
        }
        if ((line[0] == 'E') && (!bLiveRecording)) {
            int result = sscanf(line,"%*c %*10i %20li %6i %*2x %*2x", &startTime, &length);
            if (result != 2) {
                dsyslog("cMarkAdStandalone::LoadInfo(): vdr info file not valid, could not read start time and length");
                startTime = 0;
                length = 0;
            }
        }
        if (line[0] == 'T') {
            int result = sscanf(line, "%*c %79c", title);
            if ((result == 0) || (result == EOF)) {
                title[0] = 0;
            }
            else {
                char *lf = strchr(title, 10);
                if (lf) *lf = 0;
                char *cr = strchr(title, 13);
                if (cr) *cr = 0;
            }
        }
        if (line[0] == 'F') {
            int fps;
            int result = sscanf(line, "%*c %3i", &fps);
            if ((result == 0) || (result == EOF)) {
                macontext.Video.Info.framesPerSecond = 0;
            }
            else {
                macontext.Video.Info.framesPerSecond = fps;
            }
        }
        if ((line[0] == 'X') && (!bLiveRecording)) {
            int stream = 0, type = 0;
            char descr[256] = "";
            int result=sscanf(line, "%*c %3i %3i %250c", &stream, &type, (char *) &descr);
            if ((result != 0) && (result != EOF)) {
                if ((stream == 1) || (stream == 5)) {
                    if ((type != 1) && (type != 5) && (type != 9) && (type != 13)) {
                        isyslog("broadcast aspect ratio 16:9 (from vdr info)");
                        macontext.Info.AspectRatio.num = 16;
                        macontext.Info.AspectRatio.den = 9;
                    }
                    else {
                        isyslog("broadcast aspect ratio 4:3 (from vdr info)");
                        macontext.Info.AspectRatio.num = 4;
                        macontext.Info.AspectRatio.den = 3;
                    }
                }

                if (stream == 2) {
                    if (type == 5) {
                        // if we have DolbyDigital 2.0 disable AC3
                        if (strchr(descr, '2')) {
                            isyslog("broadcast with DolbyDigital2.0 (from vdr info)");
                            macontext.Info.Channels[stream] = 2;
                        }
                        // if we have DolbyDigital 5.1 disable video decoding
                        if (strchr(descr, '5')) {
                            isyslog("broadcast with DolbyDigital5.1 (from vdr info)");
                            macontext.Info.Channels[stream] = 6;
                        }
                    }
                }
            }
        }
    }
    if ((macontext.Info.AspectRatio.num == 0) && (macontext.Info.AspectRatio.den == 0)) isyslog("no broadcast aspect ratio found in vdr info");
    if (line) free(line);

    macontext.Info.timerVPS = IsVPSTimer();
    if ((length) && (startTime)) {
        if (!bIgnoreTimerInfo) {
            time_t rStart = GetRecordingStart(startTime, fileno(f));
            if (rStart) {
                dsyslog("cMarkAdStandalone::LoadInfo(): recording start at %s", strtok(ctime(&rStart), "\n"));
                dsyslog("cMarkAdStandalone::LoadInfo():     timer start at %s", strtok(ctime(&startTime), "\n"));
                if (macontext.Info.timerVPS) { //  VPS controlled recording start, we use assume broascast start 45s after recording start
                    isyslog("VPS controlled recording start");
                    macontext.Info.tStart = marks.LoadVPS(macontext.Config->recDir, "START:"); // VPS start mark
                    if (macontext.Info.tStart >= 0) {
                        dsyslog("cMarkAdStandalone::LoadInfo(): found VPS start event at offset %ds", macontext.Info.tStart);
                    }
                    else {
                        dsyslog("cMarkAdStandalone::LoadInfo(): no VPS start event found");
                        macontext.Info.tStart = 45;
                    }
                }
                else {
                    macontext.Info.tStart = static_cast<int> (startTime - rStart);
                    if (macontext.Info.tStart > 60 * 60) {   // more than 1h pre-timer make no sense, there must be a wrong directory time
                        isyslog("pre-time %is not valid, possible wrong directory time, set pre-timer to vdr default (2min)", macontext.Info.tStart);
                        macontext.Info.tStart = 120;
                    }
                    if (macontext.Info.tStart < 0) {
                        if (length + macontext.Info.tStart > 0) {
                            startTime = rStart;
                            isyslog("missed broadcast start by %02d:%02d min, event length %5ds", -macontext.Info.tStart / 60, -macontext.Info.tStart % 60, length);
                            length += macontext.Info.tStart;
                            isyslog("                                 corrected length %5ds", length);
                        }
                        else {
                            isyslog("cannot determine broadcast start, assume VDR default pre timer of 120s");
                            macontext.Info.tStart = 120;
                        }
                    }
                }
            }
            else {
                macontext.Info.tStart = 0;
            }
        }
        else {
            macontext.Info.tStart = 0;
        }
    }
    else {
        dsyslog("cMarkAdStandalone::LoadInfo(): start time and length from vdr info file not valid");
        macontext.Info.tStart = 0;
    }
    fclose(f);
    dsyslog("cMarkAdStandalone::LoadInfo(): broadcast start %is after recording start", macontext.Info.tStart);

    if ((!length) && (!bLiveRecording)) {
        esyslog("cannot read broadcast length from info, marks can be wrong!");
        macontext.Info.AspectRatio.num = 0;
        macontext.Info.AspectRatio.den = 0;
        bDecodeVideo = macontext.Config->decodeVideo;
        macontext.Video.Options.ignoreAspectRatio = false;
    }

    if (!macontext.Info.ChannelName) {
        return false;
    }
    else {
        return true;
    }
}


bool cMarkAdStandalone::CheckTS() {
    if (!directory) return false;
    char *buf;
    if (asprintf(&buf, "%s/00001.ts", directory) == -1) return false;
    ALLOC(strlen(buf)+1, "buf");
    struct stat statbuf;
    if (stat(buf,&statbuf) == -1) {
        if (errno != ENOENT) {
            FREE(strlen(buf)+1, "buf");
            free(buf);
            return false;
        }
        buf=NULL;
    }
    FREE(strlen(buf)+1, "buf");
    free(buf);
    return true;
}


bool cMarkAdStandalone::CreatePidfile() {
    char *buf = NULL;
    if (asprintf(&buf, "%s/markad.pid", directory) == -1) return false;
    ALLOC(strlen(buf)+1, "buf");

    // check for other running markad process
    FILE *oldpid=fopen(buf, "r");
    if (oldpid) {
        // found old pidfile, check if it's still running
        int pid;
        if (fscanf(oldpid, "%10i\n", &pid) == 1) {
            char procname[256] = "";
            snprintf(procname, sizeof(procname), "/proc/%i",pid);
            struct stat statbuf;
            if (stat(procname,&statbuf) != -1) {
                // found another, running markad
                fprintf(stderr, "another instance is running on %s", directory);
                abortNow = duplicate = true;
            }
        }
        fclose(oldpid);
    }
    else { // fopen above sets the error to 2, reset it here!
        errno = 0;
    }
    if (duplicate) {
        FREE(strlen(buf)+1, "buf");
        free(buf);
        return false;
    }

    FILE *pidfile = fopen(buf, "w+");

    SetFileUID(buf);

    FREE(strlen(buf)+1, "buf");
    free(buf);
    if (!pidfile) return false;
    fprintf(pidfile, "%i\n", static_cast<int> (getpid()));
    fflush(pidfile);
    fclose(pidfile);
    return true;
}


void cMarkAdStandalone::RemovePidfile() {
    if (!directory) return;
    if (duplicate) return;

    char *buf;
    if (asprintf(&buf, "%s/markad.pid", directory) != -1) {
        ALLOC(strlen(buf)+1, "buf");
        unlink(buf);
        FREE(strlen(buf)+1, "buf");
        free(buf);
    }
}


// const char cMarkAdStandalone::frametypes[8]={'?','I','P','B','D','S','s','b'};


cMarkAdStandalone::cMarkAdStandalone(const char *directoryParam, sMarkAdConfig *config, cIndex *recordingIndex) {
    setlocale(LC_MESSAGES, "");
    directory = directoryParam;
    gotendmark = false;
    inBroadCast = false;
    iStopinBroadCast = false;
    isREEL = false;
    recordingIndexMark = recordingIndex;
    marks.RegisterIndex(recordingIndexMark);
    indexFile = NULL;
    video = NULL;
    audio = NULL;
    osd = NULL;

    length = 0;
    sleepcnt = 0;
    waittime = iwaittime = 0;
    duplicate = false;
    title[0] = 0;

    macontext = {};
    macontext.Config = config;

    bDecodeVideo = config->decodeVideo;
    bDecodeAudio = config->decodeAudio;

    macontext.Info.tStart = iStart = iStop = iStopA = 0;

    if ((config->ignoreInfo & IGNORE_TIMERINFO) == IGNORE_TIMERINFO) {
        bIgnoreTimerInfo = true;
    }
    else {
        bIgnoreTimerInfo = false;
    }

    if (!config->noPid) {
        CreatePidfile();
        if (abortNow) return;
    }

    if (LOG2REC) {
        char *fbuf;
        if (asprintf(&fbuf, "%s/%s", directory, config->logFile) != -1) {
            ALLOC(strlen(fbuf)+1, "fbuf");
            if (freopen(fbuf, "w+", stdout)) {};
            SetFileUID(fbuf);
            FREE(strlen(fbuf)+1, "fbuf");
            free(fbuf);
        }
    }

    long lb;
    errno = 0;
    lb=sysconf(_SC_LONG_BIT);
    if (errno == 0) isyslog("starting markad v%s (%libit)", VERSION, lb);
    else isyslog("starting markad v%s", VERSION);

    int ver = avcodec_version();
    char *libver = NULL;
    if (asprintf(&libver, "%i.%i.%i", ver >> 16 & 0xFF, ver >> 8 & 0xFF, ver & 0xFF) != -1) {
        ALLOC(strlen(libver)+1, "libver");
        isyslog("using libavcodec.so.%s with %i threads", libver, config->threads);
        if (ver!=LIBAVCODEC_VERSION_INT) {
            esyslog("libavcodec header version %s", AV_STRINGIFY(LIBAVCODEC_VERSION));
            esyslog("header and library mismatch, do not report decoder bugs");
        }
        if ((ver >> 16) < LIBAVCODEC_VERSION_MIN) {
            esyslog("your libavcodec is not supported, update libavcodec to at least version %d", LIBAVCODEC_VERSION_MIN);
            exit(1);
        }
        if ((ver >> 16) == LIBAVCODEC_VERSION_MIN) esyslog("your libavcodec is deprecated, update libavcodec to at least version %d, do not report decoder bugs", LIBAVCODEC_VERSION_MIN + 1);
        FREE(strlen(libver)+1, "libver");
        free(libver);
    }
    tsyslog("libavcodec config: %s",avcodec_configuration());
    isyslog("on %s", directory);

    if (!bDecodeAudio) {
        isyslog("audio decoding disabled by user");
    }
    if (!bDecodeVideo) {
        isyslog("video decoding disabled by user");
    }
    if (bIgnoreTimerInfo) {
        isyslog("timer info usage disabled by user");
    }
    if (config->logoExtraction != -1) {
        // just to be sure extraction works
        bDecodeVideo = true;
    }
    if (config->before) sleep(10);

    char *tmpDir = strdup(directory);
#ifdef DEBUG_MEM
    ALLOC(strlen(tmpDir)+1, "tmpDir");
    int memsize_tmpDir = strlen(directory) + 1;
#endif
    char *datePart = strrchr(tmpDir, '/');
    if (!datePart) {
        dsyslog("cMarkAdStandalone::cMarkAdStandalone(): faild to find last '/'");
        FREE(strlen(tmpDir+1), "tmpDir");
        free(tmpDir);
        return;
    }
    *datePart = 0;    // cut off date part
    char *recName = strrchr(tmpDir, '/');
    if (!recName) {
        dsyslog("cMarkAdStandalone::cMarkAdStandalone(): faild to find last '/'");
        FREE(strlen(tmpDir+1), "tmpDir");
        free(tmpDir);
        return;
    }
    if (strstr(recName, "/@")) {
        isyslog("live-recording, disabling pre-/post timer");
        bIgnoreTimerInfo = true;
        bLiveRecording = true;
    }
    else {
        bLiveRecording = false;
    }
#ifdef DEBUG_MEM
    FREE(memsize_tmpDir, "tmpDir");
#endif
    free(tmpDir);

    if (!CheckTS()) {
        esyslog("no files found");
        abortNow = true;
        return;
    }

    if (asprintf(&indexFile, "%s/index", directory) == -1) indexFile = NULL;
    ALLOC(strlen(indexFile)+1, "indexFile");

    if (!LoadInfo()) {
        if (bDecodeVideo) {
            esyslog("failed loading info - logo %s%sdisabled",
                    (config->logoExtraction != -1) ? "extraction" : "detection",
                    bIgnoreTimerInfo ? " " : " and pre-/post-timer ");
            macontext.Info.tStart = iStart = iStop = iStopA = 0;
            macontext.Video.Options.ignoreLogoDetection = true;
        }
    }
    else {
        if (!CheckLogo() && (config->logoExtraction==-1) && (config->autoLogo == 0)) {
            isyslog("no logo found, logo detection disabled");
            macontext.Video.Options.ignoreLogoDetection = true;
        }
    }

    if (macontext.Info.tStart > 1) {
        if ((macontext.Info.tStart < 60) && (!macontext.Info.timerVPS)) macontext.Info.tStart = 60;
    }
    isyslog("pre-timer %is", macontext.Info.tStart);

    if (length) isyslog("broadcast length %imin", static_cast<int> (round(length / 60)));

    if (title[0]) {
        ptitle = title;
    }
    else {
        ptitle = (char *) directory;
    }

    if (config->osd) {
        osd= new cOSDMessage(config->svdrphost, config->svdrpport);
        if (osd) osd->Send("%s '%s'", tr("starting markad for"), ptitle);
    }
    else {
        osd = NULL;
    }

    if (config->markFileName[0]) marks.SetFileName(config->markFileName);

    if (!abortNow) {
        video = new cMarkAdVideo(&macontext, recordingIndex);
        ALLOC(sizeof(*video), "video");
        audio = new cMarkAdAudio(&macontext, recordingIndex);
        ALLOC(sizeof(*audio), "audio");
        if (macontext.Info.ChannelName)
            isyslog("channel %s", macontext.Info.ChannelName);
        if (macontext.Info.vPidType == MARKAD_PIDTYPE_VIDEO_H264)
            macontext.Video.Options.ignoreAspectRatio = true;
    }

    framecnt1 = 0;
    framecnt2 = 0;
    framecnt3 = 0;
    framecnt4 = 0;
    chkSTART = chkSTOP = INT_MAX;
}


cMarkAdStandalone::~cMarkAdStandalone() {
    marks.Save(directory, &macontext, true);
    if ((!abortNow) && (!duplicate)) {
        LogSeparator();
        dsyslog("time for decoding:              %3ds %3dms", decodeTime_us / 1000000, (decodeTime_us % 1000000) / 1000);
        if (logoSearchTime_ms > 0) dsyslog("time to find logo in recording: %3ds %3dms", logoSearchTime_ms / 1000, logoSearchTime_ms % 1000);
        if (logoChangeTime_ms > 0) dsyslog("time to find logo changes:      %3ds %3dms", logoChangeTime_ms / 1000, logoChangeTime_ms % 1000);

        time_t sec = endPass1.tv_sec - startPass1.tv_sec;
        suseconds_t usec = endPass1.tv_usec - startPass1.tv_usec;
        if (usec < 0) {
            usec += 1000000;
            sec--;
        }
        if ((sec + usec / 1000000) > 0) isyslog("pass 1: time %3lds %03ldms, frames %6i, fps %6ld", sec, usec / 1000, framecnt1, framecnt1 / (sec + usec / 1000000));


        sec = endPass2.tv_sec - startPass2.tv_sec;
        usec = endPass2.tv_usec - startPass2.tv_usec;
        if (usec < 0) {
            usec += 1000000;
            sec--;
        }
        if ((sec + usec / 1000000) > 0) isyslog("pass 2: time %3lds %03ldms, frames %6i, fps %6ld", sec, usec / 1000, framecnt2, framecnt2 / (sec + usec / 1000000));

        sec = endPass3.tv_sec - startPass3.tv_sec;
        usec = endPass3.tv_usec - startPass3.tv_usec;
        if (usec < 0) {
            usec += 1000000;
            sec--;
        }
        if ((sec + usec / 1000000) > 0) isyslog("pass 3: time %3lds %03ldms, frames %6i, fps %6ld", sec, usec / 1000, framecnt3, framecnt3 / (sec + usec / 1000000));

        sec = endPass4.tv_sec - startPass4.tv_sec;
        usec = endPass4.tv_usec - startPass4.tv_usec;
        if (usec < 0) {
            usec += 1000000;
            sec--;
        }
        if ((sec + usec / 1000000) > 0) isyslog("pass 4: time %3lds %03ldms, frames %6i, fps %6ld", sec, usec / 1000, framecnt4, framecnt4 / (sec + usec / 1000000));

        gettimeofday(&endAll, NULL);
        sec = endAll.tv_sec - startAll.tv_sec;
        usec = endAll.tv_usec - startAll.tv_usec;
        if (usec < 0) {
            usec += 1000000;
            sec--;
        }
        double etime = 0;
        double ftime = 0;
        etime = sec + ((double) usec / 1000000) - waittime;
        if (etime > 0) ftime = (framecnt1 + framecnt2 + framecnt3) / etime;
        isyslog("processed time %d:%02d min with %.1f fps", static_cast<int> (etime / 60), static_cast<int> (etime - (static_cast<int> (etime / 60) * 60)), ftime);
    }

    if ((osd) && (!duplicate)) {
        if (abortNow) {
            osd->Send("%s '%s'", tr("markad aborted for"), ptitle);
        }
        else {
            osd->Send("%s '%s'", tr("markad finished for"), ptitle);
        }
    }

    if (macontext.Info.ChannelName) {
        FREE(strlen(macontext.Info.ChannelName)+1, "macontext.Info.ChannelName");
        free(macontext.Info.ChannelName);
    }
    if (indexFile) {
        FREE(strlen(indexFile)+1, "indexFile");
        free(indexFile);
    }
    if (video) {
        FREE(sizeof(*video), "video");
        delete video;
        video = NULL;
    }
    if (audio) {
        FREE(sizeof(*audio), "audio");
        delete audio;
        audio = NULL;
    }
    if (osd) {
        FREE(sizeof(*osd), "osd");
        delete osd;
        osd = NULL;
    }
    if (ptr_cDecoder) {
        if (ptr_cDecoder->GetErrorCount() > 0) esyslog("decoding errors: %d", ptr_cDecoder->GetErrorCount());
        FREE(sizeof(*ptr_cDecoder), "ptr_cDecoder");
        delete ptr_cDecoder;
        ptr_cDecoder = NULL;
    }
    RemovePidfile();
}


bool isnumber(const char *s) {
    while (*s) {
        if (!isdigit(*s))
            return false;
        s++;
    }
    return true;
}


int usage(int svdrpport) {
    // nothing done, give the user some help
    printf("Usage: markad [options] cmd <record>\n"
           "options:\n"
           "-b              --background\n"
           "                  markad runs as a background-process\n"
           "                  this will be automatically set if called with \"after\"\n"
           "-d              --disable=<option>\n"
           "                  <option>   1 = disable video decoding, 2 = disable audio\n"
           "                             decoding, 3 = disable video and audio decoding\n"
           "-i              --ignoreinfo=<info>\n"
           "                  ignores hints from info(.vdr) file\n"
           "                  <info> 4 = ignore timer info\n"
           "-l              --logocachedir\n"
           "                  directory where logos stored, default /var/lib/markad\n"
           "-p              --priority=<priority>\n"
           "                  software priority of markad when running in background\n"
           "                  <priority> from -20...19, default 19\n"
           "-r              --ioprio=<class>[,<level>]\n"
           "                  io priority of markad when running in background\n"
           "                  <class> 1 = realtime, <level> from 0..7, default 4\n"
           "                          2 = besteffort, <level> from 0..7, default 4\n"
           "                          3 = idle (default)\n"
           "                  make sure your I/O scheduler supports scheduling priorities and classes (e.g. BFQ or CFQ)\n"
           "-v              --verbose\n"
           "                  increments loglevel by one, can be given multiple times\n"
           "-B              --backupmarks\n"
           "                  make a backup of existing marks\n"
           "-I              --saveinfo\n"
           "                  correct information in info file\n"
           "-L              --extractlogo=<direction>[,width[,height]]\n"
           "                  extracts logo to /tmp as pgm files (must be renamed)\n"
           "                  <direction>  0 = top left,    1 = top right\n"
           "                               2 = bottom left, 3 = bottom right\n"
           "-O              --OSD\n"
           "                  markad sends an OSD-Message for start and end\n"
           "-R              --log2rec\n"
           "                  write logfiles into recording directory\n"
           "                --logfile=<filename>\n"
           "                  logfile name (default: markad.log)\n"
           "-T              --threads=<number>\n"
           "                  number of threads used for decoding, max. 16\n"
           "                  (default is the number of cpus)\n"
           "-V              --version\n"
           "                  print version-info and exit\n"
           "                --loglevel=<level>\n"
           "                  sets loglevel to the specified value\n"
           "                  <level> 1=error 2=info 3=debug 4=trace\n"
           "                --markfile=<markfilename>\n"
           "                  set a different markfile-name\n"
           "                --nopid\n"
           "                  disables creation of markad.pid file in recdir\n"
           "                --online[=1|2] (default is 1)\n"
           "                  start markad immediately when called with \"before\" as cmd\n"
           "                  if online is 1, markad starts online for live-recordings\n"
           "                  only, online=2 starts markad online for every recording\n"
           "                  live-recordings are identified by having a '@' in the\n"
           "                  filename so the entry 'Mark instant recording' in the menu\n"
           "                  'Setup - Recording' of the vdr should be set to 'yes'\n"
           "                --pass1only\n"
           "                  process only first pass, setting of marks\n"
           "                --pass2only\n"
           "                  process only second pass, fine adjustment of marks\n"
           "                --svdrphost=<ip/hostname> (default is 127.0.0.1)\n"
           "                  ip/hostname of a remote VDR for OSD messages\n"
           "                --svdrpport=<port> (default is %i)\n"
           "                  port of a remote VDR for OSD messages\n"
           "                --astopoffs=<value> (default is 0)\n"
           "                  assumed stop offset in seconds range from 0 to 240\n"
           "                --posttimer=<value> (default is 600)\n"
           "                  additional recording after timer end in seconds range from 0 to 1200\n"
           "                --vps\n"
           "                  use markad.vps from recording directory to optimize start, stop and break marks\n"
           "                --cut\n"
           "                  cut vidio based on marks and write it in the recording directory\n"
           "                --ac3reencode\n"
           "                  re-encode AC3 stream to fix low audio level of cutted video on same devices\n"
           "                  requires --cut\n"
           "                --autologo=<option>\n"
           "                  <option>   0 = disable, only use logos from logo cache directory\n"
           "                             1 = deprecated, do not use\n"
           "                             2 = enable, find logo from recording and store it in the recording directory (default)\n"
           "                                 speed optimized operation mode, use it only on systems with >= 1 GB main memory\n"
           "                --fulldecode\n"
           "                  decode all video frame types and set mark position to all frame types\n"
           "                --fullencode=<streams>\n"
           "                  full reencode video generated by --cut\n"
           "                  use it only on powerfull CPUs, it will double overall run time\n"
           "                  <streams>  all  = keep all video and audio streams of the recording\n"
           "                             best = only encode best video and best audio stream, drop rest\n"
           "\ncmd: one of\n"
           "-                            dummy-parameter if called directly\n"
           "nice                         runs markad directly and with nice(19)\n"
           "after                        markad started by vdr after the recording is complete\n"
           "before                       markad started by vdr before the recording is complete, only valid together with --online\n"
           "edited                       markad started by vdr in edit function and exits immediately\n"
           "\n<record>                     is the name of the directory where the recording\n"
           "                             is stored\n\n",
           svdrpport
          );
    return -1;
}


static void signal_handler(int sig) {
    void *trace[32];
    char **messages = (char **)NULL;
    int i, trace_size = 0;

    switch (sig) {
        case SIGTSTP:
            isyslog("paused by signal");
            kill(getpid(), SIGSTOP);
            break;
        case SIGCONT:
            isyslog("continued by signal");
            break;
        case SIGABRT:
            esyslog("aborted by signal");
            abortNow = true;;
            break;
        case SIGSEGV:
            esyslog("segmentation fault");

            trace_size = backtrace(trace, 32);
            messages = backtrace_symbols(trace, trace_size);
            esyslog("[bt] Execution path:");
            for (i=0; i < trace_size; ++i) {
                esyslog("[bt] %s", messages[i]);
            }
            _exit(1);
            break;
        case SIGTERM:
        case SIGINT:
            esyslog("aborted by user");
            abortNow = true;
            break;
        default:
            break;
    }
}


char *recDir = NULL;


void freedir(void) {
    if (recDir) free(recDir);
}


int main(int argc, char *argv[]) {
    bool bAfter = false, bEdited = false;
    bool bFork = false, bNice = false, bImmediateCall = false;
    int niceLevel = 19;
    int ioprio_class = 3;
    int ioprio = 7;
    char *tok,*str;
    int ntok;
    bool bPass2Only = false;
    bool bPass1Only = false;
    struct sMarkAdConfig config = {};

    gettimeofday(&startAll, NULL);

    // set defaults
    config.decodeVideo = true;
    config.decodeAudio = true;
    config.saveInfo = false;
    config.logoExtraction = -1;
    config.logoWidth = -1;
    config.logoHeight = -1;
    config.threads = -1;
    config.astopoffs = 0;
    config.posttimer = 600;
    strcpy(config.svdrphost, "127.0.0.1");
    strcpy(config.logoDirectory, "/var/lib/markad");

    struct servent *serv=getservbyname("svdrp", "tcp");
    if (serv) {
        config.svdrpport = htons(serv->s_port);
    }
    else {
        config.svdrpport = 2001;
    }

    atexit(freedir);

    while (1) {
        int option_index = 0;
        static struct option long_options[] =
        {
            {"background", 0, 0, 'b'},
            {"disable", 1, 0, 'd'},
            {"ignoreinfo", 1, 0, 'i' },
            {"logocachedir", 1, 0, 'l'},
            {"priority",1,0,'p'},
            {"ioprio",1,0,'r'},
            {"verbose", 0, 0, 'v'},

            {"backupmarks", 0, 0, 'B'},
            {"saveinfo",0, 0, 'I'},
            {"extractlogo", 1, 0, 'L'},
            {"OSD",0,0,'O' },
            {"log2rec",0,0,'R'},
            {"threads", 1, 0, 'T'},
            {"version", 0, 0, 'V'},

            {"markfile",1,0,1},
            {"loglevel",1,0,2},
            {"online",2,0,3},
            {"nopid",0,0,4},
            {"svdrphost",1,0,5},
            {"svdrpport",1,0,6},
            {"pass2only",0,0,7},
            {"pass1only",0,0,8},
            {"astopoffs",1,0,9},
            {"posttimer",1,0,10},
            {"cDecoder",0,0,11},
            {"cut",0,0,12},
            {"ac3reencode",0,0,13},
            {"vps",0,0,14},
            {"logfile",1,0,15},
            {"autologo",1,0,16},
            {"fulldecode",0,0,17},
            {"fullencode",1,0,18},

            {0, 0, 0, 0}
        };

        int option = getopt_long  (argc, argv, "bd:i:l:p:r:vBGIL:ORT:V", long_options, &option_index);
        if (option == -1) break;

        switch (option) {
            case 'b':
                // --background
                bFork = SYSLOG = true;
                break;
            case 'd':
                // --disable
                switch (atoi(optarg)) {
                    case 1:
                        config.decodeVideo = false;
                        break;
                    case 2:
                        config.decodeAudio = false;
                        break;
                    case 3:
                        config.decodeVideo = false;
                        config.decodeAudio = false;
                        break;
                    default:
                        fprintf(stderr, "markad: invalid disable option: %s\n", optarg);
                         return 2;
                         break;
                }
                break;
            case 'i':
                // --ignoreinfo
                config.ignoreInfo = atoi(optarg);
                if ((config.ignoreInfo < 1) || (config.ignoreInfo > 255)) {
                    fprintf(stderr, "markad: invalid ignoreinfo option: %s\n", optarg);
                    return 2;
                }
                break;
            case 'l':
                strncpy(config.logoDirectory, optarg, sizeof(config.logoDirectory));
                config.logoDirectory[sizeof(config.logoDirectory) - 1]=0;
                break;
            case 'p':
                // --priority
                if (isnumber(optarg) || *optarg == '-') niceLevel = atoi(optarg);
                else {
                    fprintf(stderr, "markad: invalid priority level: %s\n", optarg);
                    return 2;
                }
                bNice = true;
                break;
            case 'r':
                // --ioprio
                str=strchr(optarg, ',');
                if (str) {
                    *str = 0;
                    ioprio = atoi(str+1);
                    *str = ',';
                }
                ioprio_class = atoi(optarg);
                if ((ioprio_class < 1) || (ioprio_class > 3)) {
                    fprintf(stderr, "markad: invalid io-priority: %s\n", optarg);
                    return 2;
                }
                if ((ioprio < 0) || (ioprio > 7)) {
                    fprintf(stderr, "markad: invalid io-priority: %s\n", optarg);
                    return 2;
                }
                if (ioprio_class == 3) ioprio = 7;
                bNice = true;
                break;
            case 'v':
                // --verbose
                SysLogLevel++;
                if (SysLogLevel > 10) SysLogLevel = 10;
                break;
            case 'B':
                // --backupmarks
                config.backupMarks = true;
                break;
            case 'I':
                config.saveInfo = true;
                break;
            case 'L':
                // --extractlogo
                str=optarg;
                ntok=0;
                while (true) {
                    tok=strtok(str, ",");
                    if (!tok) break;
                    switch (ntok) {
                        case 0:
                            config.logoExtraction = atoi(tok);
                            if ((config.logoExtraction < 0) || (config.logoExtraction > 3)) {
                                fprintf(stderr, "markad: invalid extractlogo value: %s\n", tok);
                                return 2;
                            }
                            break;
                        case 1:
                            config.logoWidth = atoi(tok);
                            break;
                        case 2:
                            config.logoHeight = atoi(tok);
                            break;
                         default:
                            break;
                    }
                    str = NULL;
                    ntok++;
                }
                break;
            case 'O':
                // --OSD
                config.osd = true;
                break;
            case 'R':
                // --log2rec
                LOG2REC = true;
                break;
            case 'T':
                // --threads
                config.threads = atoi(optarg);
                if (config.threads < 1) config.threads = 1;
                if (config.threads > 16) config.threads = 16;
                break;
            case 'V':
                printf("markad %s - marks advertisements in VDR recordings\n", VERSION);
                return 0;
            case '?':
                printf("unknown option ?\n");
                break;
            case 0:
                printf ("option %s", long_options[option_index].name);
                if (optarg) printf (" with arg %s", optarg);
                printf ("\n");
                break;
            case 1: // --markfile
                strncpy(config.markFileName, optarg, sizeof(config.markFileName));
                config.markFileName[sizeof(config.markFileName) - 1] = 0;
                break;
            case 2: // --loglevel
                SysLogLevel = atoi(optarg);
                if (SysLogLevel > 10) SysLogLevel = 10;
                if (SysLogLevel < 0) SysLogLevel = 2;
                break;
            case 3: // --online
                if (optarg) {
                    config.online = atoi(optarg);
                }
                else {
                    config.online = 1;
                }
                if ((config.online != 1) && (config.online != 2)) {
                    fprintf(stderr, "markad: invalid online value: %s\n", optarg);
                    return 2;
                }
                break;
            case 4: // --nopid
                config.noPid = true;
                break;
            case 5: // --svdrphost
                strncpy(config.svdrphost, optarg, sizeof(config.svdrphost));
                config.svdrphost[sizeof(config.svdrphost) - 1] = 0;
                break;
            case 6: // --svdrpport
                if (isnumber(optarg) && atoi(optarg) > 0 && atoi(optarg) < 65536) {
                    config.svdrpport = atoi(optarg);
                }
                else {
                    fprintf(stderr, "markad: invalid svdrpport value: %s\n", optarg);
                    return 2;
                }
                break;
            case 7: // --pass2only
                bPass2Only = true;
                if (bPass1Only) {
                    fprintf(stderr, "markad: you cannot use --pass2only with --pass1only\n");
                    return 2;
                }
                break;
            case 8: // --pass1only
                bPass1Only = true;
                if (bPass2Only) {
                    fprintf(stderr, "markad: you cannot use --pass1only with --pass2only\n");
                    return 2;
                }
                break;
            case 9: // --astopoffs
                if (isnumber(optarg) && atoi(optarg) >= 0 && atoi(optarg) <= 240) {
                    config.astopoffs = atoi(optarg);
                }
                else {
                    fprintf(stderr, "markad: invalid astopoffs value: %s\n", optarg);
                    return 2;
                }
                break;
            case 10: // --posttimer
                if (isnumber(optarg) && atoi(optarg) >= 0 && atoi(optarg) <= 1200) config.posttimer=atoi(optarg);
                else {
                    fprintf(stderr, "markad: invalid posttimer value: %s\n", optarg);
                    return 2;
                }
                break;
            case 11: // --cDecoder
                fprintf(stderr, "markad: parameter --cDecoder: is depreciated, please remove it from your configuration\n");
                break;
            case 12: // --cut
                config.MarkadCut = true;
                break;
            case 13: // --ac3reencode
                config.ac3ReEncode = true;
                break;
            case 14: // --vps
                config.useVPS = true;
                break;
            case 15: // --logfile
                strncpy(config.logFile, optarg, sizeof(config.logFile));
                config.logFile[sizeof(config.logFile) - 1] = 0;
                break;
            case 16: // --autologo
                if (isnumber(optarg) && atoi(optarg) >= 0 && atoi(optarg) <= 2) config.autoLogo = atoi(optarg);
                else {
                    fprintf(stderr, "markad: invalid autologo value: %s\n", optarg);
                    return 2;
                }
                if (config.autoLogo == 1) {
                    fprintf(stderr,"markad: --autologo=1 is removed, will use --autologo=2 instead, please update your configuration\n");
                    config.autoLogo = 2;
                }
                break;
            case 17: // --fulldecode
                config.fullDecode = true;
                break;
            case 18: // --fullencode
                config.fullEncode = true;
                str = optarg;
                ntok = 0;
                while (str) {
                    tok = strtok(str, ",");
                    if (!tok) break;
                    switch (ntok) {
                        case 0:
                            if (strcmp(tok, "all") == 0) config.bestEncode = false;
                            else if (strcmp(tok, "best") == 0) config.bestEncode = true;
                                 else {
                                     fprintf(stderr, "markad: invalid --fullencode value: %s\n", tok);
                                     return 2;
                                 }
                            break;
                         default:
                            break;
                    }
                    str = NULL;
                    ntok++;
                }
                break;
            default:
                printf ("? getopt returned character code 0%o ? (option_index %d)\n", option,option_index);
        }
    }

    if (optind < argc) {
        while (optind < argc) {
            if (strcmp(argv[optind], "after" ) == 0 ) {
                bAfter = bFork = bNice = SYSLOG = true;
            }
            else if (strcmp(argv[optind], "before" ) == 0 ) {
                if (!config.online) config.online = 1;
                config.before = bFork = bNice = SYSLOG = true;
            }
            else if (strcmp(argv[optind], "edited" ) == 0 ) {
                bEdited = true;
            }
            else if (strcmp(argv[optind], "nice" ) == 0 ) {
                bNice = true;
            }
            else if (strcmp(argv[optind], "-" ) == 0 ) {
                bImmediateCall = true;
            }
            else {
                if ( strstr(argv[optind], ".rec") != NULL ) {
                    recDir=realpath(argv[optind], NULL);
                    config.recDir = recDir;
                }
            }
            optind++;
        }
    }

    // set defaults
    if (config.logFile[0] == 0) {
        strncpy(config.logFile, "markad.log", sizeof(config.logFile));
        config.logFile[sizeof("markad.log") - 1] = 0;
    }

    // do nothing if called from vdr before/after the video is cutted
    if (bEdited) return 0;
    if ((bAfter) && (config.online)) return 0;
    if ((config.before) && (config.online == 1) && recDir && (!strchr(recDir, '@'))) return 0;

    // we can run, if one of bImmediateCall, bAfter, bBefore or bNice is true
    // and recDir is given
    if ( (bImmediateCall || config.before || bAfter || bNice) && recDir ) {
        // if bFork is given go in background
        if ( bFork ) {
            //close_files();
            pid_t pid = fork();
            if (pid < 0) {
                char *err = strerror(errno);
                fprintf(stderr, "%s\n", err);
                return 2;
            }
            if (pid != 0) {
                return 0; // initial program immediately returns
            }
            if (chdir("/") == -1) {
                perror("chdir");
                exit(EXIT_FAILURE);
            }
            if (setsid() == (pid_t)(-1)) {
                perror("setsid");
                exit(EXIT_FAILURE);
            }
            if (signal(SIGHUP, SIG_IGN) == SIG_ERR) {
                perror("signal(SIGHUP, SIG_IGN)");
                errno = 0;
            }
            int f;

            f = open("/dev/null", O_RDONLY);
            if (f == -1) {
                perror("/dev/null");
                errno = 0;
            }
            else {
                if (dup2(f, fileno(stdin)) == -1) {
                    perror("dup2");
                    errno = 0;
                }
                (void)close(f);
            }

            f = open("/dev/null", O_WRONLY);
            if (f == -1) {
                perror("/dev/null");
                errno = 0;
            }
            else {
                if (dup2(f, fileno(stdout)) == -1) {
                    perror("dup2");
                    errno = 0;
                }
                if (dup2(f, fileno(stderr)) == -1) {
                    perror("dup2");
                    errno = 0;
                }
                (void)close(f);
            }
        }

        (void)umask((mode_t)0022);

        int MaxPossibleFileDescriptors = getdtablesize();
        for (int i = STDERR_FILENO + 1; i < MaxPossibleFileDescriptors; i++)
            close(i); //close all dup'ed filedescriptors

        // should we renice ?
        if (bNice) {
            if (setpriority(PRIO_PROCESS, 0, niceLevel) == -1) {
                fprintf(stderr, "failed to set nice to %d\n", niceLevel);
            }
            if (ioprio_set(1,getpid(), ioprio | ioprio_class << 13) == -1) {
                fprintf(stderr, "failed to set ioprio to %i,%i\n", ioprio_class, ioprio);
            }
        }
        // store the real values, maybe set by calling nice
        errno = 0;
        int PrioProcess = getpriority(PRIO_PROCESS, 0);
        if ( errno ) {  // use errno because -1 is a valid return value
            fprintf(stderr,"failed to get nice value\n");
        }
        int IOPrio = ioprio_get(1, getpid());
        if (! IOPrio) {
            fprintf(stderr,"failed to get ioprio\n");
        }
        IOPrio = IOPrio >> 13;

        // now do the work...
        struct stat statbuf;
        if (stat(recDir, &statbuf) == -1) {
            fprintf(stderr,"%s not found\n", recDir);
            return -1;
        }

        if (!S_ISDIR(statbuf.st_mode)) {
            fprintf(stderr, "%s is not a directory\n", recDir);
            return -1;
        }

        if (access(recDir, W_OK|R_OK) == -1) {
            fprintf(stderr,"cannot access %s\n", recDir);
            return -1;
        }

        // ignore some signals
        signal(SIGHUP, SIG_IGN);

        // catch some signals
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        signal(SIGSEGV, signal_handler);
        signal(SIGABRT, signal_handler);
        signal(SIGUSR1, signal_handler);
        signal(SIGTSTP, signal_handler);
        signal(SIGCONT, signal_handler);

        cIndex *recordingIndex = new cIndex();
        ALLOC(sizeof(*recordingIndex), "recordingIndex");

        cmasta = new cMarkAdStandalone(recDir,&config, recordingIndex);
        ALLOC(sizeof(*cmasta), "cmasta");
        if (!cmasta) return -1;

        isyslog("parameter --loglevel is set to %i", SysLogLevel);

        if (niceLevel != 19) {
            isyslog("parameter --priority %i", niceLevel);
            isyslog("warning: increasing priority may affect other applications");
        }
        if (ioprio_class != 3) {
            isyslog("parameter --ioprio %i", ioprio_class);
            isyslog("warning: increasing priority may affect other applications");
        }
        dsyslog("markad process nice level %i", PrioProcess);
        dsyslog("markad IO priority class  %i" ,IOPrio);

        dsyslog("parameter --logocachedir is set to %s", config.logoDirectory);
        dsyslog("parameter --threads is set to %i", config.threads);
        dsyslog("parameter --astopoffs is set to %i", config.astopoffs);
        if (LOG2REC) dsyslog("parameter --log2rec is set");

        if (config.useVPS) {
            dsyslog("parameter --vps is set");
        }
        if (config.MarkadCut) {
            dsyslog("parameter --cut is set");
        }
        if (config.ac3ReEncode) {
            dsyslog("parameter --ac3reencode is set");
            if (!config.MarkadCut) {
                esyslog("--cut is not set, ignoring --ac3reencode");
                config.ac3ReEncode = false;
            }
        }
        dsyslog("parameter --autologo is set to %i",config.autoLogo);
        if (config.fullDecode) {
            dsyslog("parameter --fulldecode is set");
        }
        if (config.fullEncode) {
            dsyslog("parameter --fullencode is set");
            if (config.bestEncode) dsyslog("encode best streams");
            else dsyslog("encode all streams");
        }
        if (!bPass2Only) {
            gettimeofday(&startPass1, NULL);
            cmasta->ProcessFiles();
            gettimeofday(&endPass1, NULL);
        }
        if (!bPass1Only) {
            gettimeofday(&startPass2, NULL);
            cmasta->Process2ndPass();  // overlap detection
            gettimeofday(&endPass2, NULL);

            gettimeofday(&startPass3, NULL);
            cmasta->Process3ndPass();  // Audio silence detection
            gettimeofday(&endPass3, NULL);
        }
        if (config.MarkadCut) {
            gettimeofday(&startPass4, NULL);
            cmasta->MarkadCut();
            gettimeofday(&endPass4, NULL);
        }
#ifdef DEBUG_MARK_FRAMES
        cmasta->DebugMarkFrames(); // write frames picture of marks to recording directory
#endif
        if (cmasta) {
            FREE(sizeof(*cmasta), "cmasta");
            delete cmasta;
            cmasta = NULL;
        }
        if (recordingIndex) {
            FREE(sizeof(*recordingIndex), "recordingIndex");
            delete recordingIndex;
            recordingIndex = NULL;
        }

#ifdef DEBUG_MEM
        memList();
#endif
        return 0;
    }
    return usage(config.svdrpport);
}
