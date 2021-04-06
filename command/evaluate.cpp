/*
 * evaluate.cpp: A program for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */


#include "evaluate.h"


// evaluate logo stop/start pairs
// used by logo change detection
cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(clMarks *marks, clMarks *blackMarks, const int framesPerSecond, const int iStart, const int chkSTART, const int iStopA) {
    if (!marks) return;

#define LOGO_CHANGE_NEXT_STOP_MIN   7  // in s, do not increase, 7s is the shortest found distance between two logo changes
                                       // next stop max (=lenght next valid broadcast) found: 1242
#define LOGO_CHANGE_STOP_START_MIN 10  // min time in s of a logo change section
#define LOGO_CHANGE_STOP_START_MAX 21  // max time in s of a logo change section


#define LOGO_CHANGE_IS_ADVERTISING_MIN 300  // in s
#define LOGO_CHANGE_IS_BROADCAST_MIN 240  // in s

    dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): start with iStart %d, chkSTART %d, iStopA %d", iStart, chkSTART, iStopA);
    logoStopStartPairType newPair;

    clMark *mark = marks->GetFirst();
    while (mark) {
        if (mark->type == MT_LOGOSTOP) newPair.stopPosition = mark->position;
        if ((mark->type == MT_LOGOSTART) && (newPair.stopPosition >= 0)) {
            newPair.startPosition = mark->position;
            logoPairVector.push_back(newPair);
            ALLOC(sizeof(logoStopStartPairType), "logoPairVector");
            // reset for next pair
            newPair.stopPosition = -1;
            newPair.startPosition = -1;
        }
        mark = mark->Next();
    }

// evaluate stop/start pairs
    for (std::vector<logoStopStartPairType>::iterator logoPairIterator = logoPairVector.begin(); logoPairIterator != logoPairVector.end(); ++logoPairIterator) {
        // check for info logo section
        isInfoLogo(blackMarks, &(*logoPairIterator), framesPerSecond);

        // mark after pair
        clMark *markStop_AfterPair = marks->GetNext(logoPairIterator->stopPosition, MT_LOGOSTOP);

        // check length of stop/start logo pair
        int deltaStopStart = (logoPairIterator->startPosition - logoPairIterator->stopPosition ) / framesPerSecond;
        if ((deltaStopStart < LOGO_CHANGE_STOP_START_MIN) &&  (logoPairIterator->isInfoLogo == -1)) { // do not change posible logo info parts
            dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): ----- stop (%d) start (%d) pair: delta too small %ds (expect >=%ds)", logoPairIterator->stopPosition, logoPairIterator->startPosition, deltaStopStart, LOGO_CHANGE_STOP_START_MIN);
            // maybe we have a wrong start/stop pait inbetween, try next start mark
            clMark *markNextStart = marks->GetNext(logoPairIterator->startPosition, MT_LOGOSTART);
            if (markNextStart) {
                int deltaStopStartNew = (markNextStart->position - logoPairIterator->stopPosition ) / framesPerSecond;
                if (deltaStopStartNew > LOGO_CHANGE_STOP_START_MAX) {
                    dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): next start mark (%d) too far away",  markNextStart->position);
                }
                else {
                    dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): replace start mark with next start mark (%d)",  markNextStart->position);
                    logoPairIterator->startPosition = markNextStart->position;
                    deltaStopStart = deltaStopStartNew;
                }
            }
            else logoPairIterator->isLogoChange = -1;
        }
        if (deltaStopStart > LOGO_CHANGE_STOP_START_MAX) {
            dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): ----- stop (%d) start (%d) pair: delta too big %ds (expect <=%ds)", logoPairIterator->stopPosition, logoPairIterator->startPosition, deltaStopStart, LOGO_CHANGE_STOP_START_MAX);
            logoPairIterator->isLogoChange = -1;
        }
        if (deltaStopStart >= LOGO_CHANGE_IS_ADVERTISING_MIN) {
            dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): ----- stop (%d) start (%d) pair: delta %ds (expect >=%ds) is a advertising", logoPairIterator->stopPosition, logoPairIterator->startPosition, deltaStopStart, LOGO_CHANGE_IS_ADVERTISING_MIN);
            logoPairIterator->isAdvertising = 1;
        }

        // check next stop distance after stop/start pair
        int delta_Stop_AfterPair = 0;
        if (markStop_AfterPair) {  // we have a next logo stop
            delta_Stop_AfterPair = (markStop_AfterPair->position - logoPairIterator->startPosition) / framesPerSecond;
        }
        else {  // this is the last logo stop we have
            if (iStart > 0) { // we were called by CheckStart, the next stop is not yet detected
                int diff = (chkSTART - logoPairIterator->stopPosition) / framesPerSecond; // difference to current processed frame
                if (diff > LOGO_CHANGE_IS_BROADCAST_MIN) delta_Stop_AfterPair = diff;     // still no stop mark but we are in broadcast
                else delta_Stop_AfterPair = LOGO_CHANGE_NEXT_STOP_MIN; // we can not ignore early stop start pairs because they can be logo changed short after start
            }
            else { // we are called by CheckStop()
                if (logoPairIterator->stopPosition < iStopA) logoPairIterator->isLogoChange = -1; // this is the last stop mark and it is before assumed end mark, this is the end mark
            }
        }
        if ((delta_Stop_AfterPair > 0) && (delta_Stop_AfterPair < LOGO_CHANGE_NEXT_STOP_MIN)) {
            dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): ----- stop (%d) start (%d) pair: stop mark after stop/start pair too fast %ds (expect >=%ds)", logoPairIterator->stopPosition, logoPairIterator->startPosition, delta_Stop_AfterPair, LOGO_CHANGE_NEXT_STOP_MIN);
            logoPairIterator->isLogoChange = -1;
        }
        if (delta_Stop_AfterPair >= LOGO_CHANGE_IS_BROADCAST_MIN) {
            dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): ----- stop (%d) start (%d) pair: next stop mark after stop/start pair in %ds (expect >=%ds, start mark is in braoscast)", logoPairIterator->stopPosition, logoPairIterator->startPosition, delta_Stop_AfterPair, LOGO_CHANGE_IS_BROADCAST_MIN);
            logoPairIterator->isStartMarkInBroadcast = 1;
        }
    }

    // check section of stop/start pairs
    // search for part between advertising and broadcast, keep this mark, because it contains the start mark of the broadcast
    //
    for (std::vector<logoStopStartPairType>::iterator logoPairIterator = logoPairVector.begin(); logoPairIterator != logoPairVector.end(); ++logoPairIterator) {
        if (logoPairIterator->isAdvertising == 1) {  // advertising pair
            std::vector<logoStopStartPairType>::iterator nextLogoPairIterator = logoPairIterator;
            ++nextLogoPairIterator;
            if (nextLogoPairIterator != logoPairVector.end()) {
                if ((nextLogoPairIterator->isLogoChange == 0) && (nextLogoPairIterator->isStartMarkInBroadcast  == 0)){ // unknown pair
                    std::vector<logoStopStartPairType>::iterator next2LogoPairIterator = nextLogoPairIterator;
                    ++next2LogoPairIterator;
                    if (next2LogoPairIterator != logoPairVector.end()) {
                        if (next2LogoPairIterator->isStartMarkInBroadcast  == 1) { // pair with bradcast start mark
                            dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): ----- stop (%d) start (%d) pair: part between advertising and broadcast, keep this mark because it contains start mark of broadcast)", nextLogoPairIterator->stopPosition, nextLogoPairIterator->startPosition);
                            nextLogoPairIterator->isLogoChange = -1;
                        }
                        if ((next2LogoPairIterator->isLogoChange == 0) && (next2LogoPairIterator->isStartMarkInBroadcast  == 0)) { // unknown pair
                            std::vector<logoStopStartPairType>::iterator next3LogoPairIterator = next2LogoPairIterator;
                            ++next3LogoPairIterator;
                            if (next3LogoPairIterator != logoPairVector.end()) {
                                if (next3LogoPairIterator->isStartMarkInBroadcast  == 1) { // pair with bradcast start mark
                                    dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): ----- stop (%d) start (%d) pair: part between advertising and broadcast, keep this mark because it contains start mark of broadcast)", next2LogoPairIterator->stopPosition, next2LogoPairIterator->startPosition);
                                    next2LogoPairIterator->isLogoChange = -1;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    for (std::vector<logoStopStartPairType>::iterator logoPairIterator = logoPairVector.begin(); logoPairIterator != logoPairVector.end(); ++logoPairIterator) {
        dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair(): add stop (%d) start (%d) pair:", logoPairIterator->stopPosition, logoPairIterator->startPosition);
        dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair():                  isLogoChange           %2d", logoPairIterator->isLogoChange);
        dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair():                  isAdvertising          %2d", logoPairIterator->isAdvertising);
        dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair():                  isStartMarkInBroadcast %2d", logoPairIterator->isStartMarkInBroadcast);
        dsyslog("cEvaluateLogoStopStartPair::cEvaluateLogoStopStartPair():                  isInfoLogo             %2d", logoPairIterator->isInfoLogo);
    }
    nextLogoPairIterator = logoPairVector.begin();
}


cEvaluateLogoStopStartPair::~cEvaluateLogoStopStartPair() {
#ifdef DEBUG_MEM
    int size =  logoPairVector.size();
    for (int i = 0 ; i < size; i++) {
        FREE(sizeof(logoStopStartPairType), "logoPairVector");
    }
#endif
     logoPairVector.clear();
}


// check if stop/start pair could be a info logo section
//
void cEvaluateLogoStopStartPair::isInfoLogo(clMarks *blackMarks, logoStopStartPairType *logoStopStartPair, const int framesPerSecond) {
#define LOGO_INTRODUCTION_STOP_START_MIN 7  // min time in s of a info logo section, bigger values than in InfoLogo becase of seek to iFrame, changed from 8 to 7
#define LOGO_INTRODUCTION_STOP_START_MAX 17  // max time in s of a info logo section, changed from 17
    int length = (logoStopStartPair->startPosition - logoStopStartPair->stopPosition) / framesPerSecond;
    if ((length <= LOGO_INTRODUCTION_STOP_START_MAX) && (length >= LOGO_INTRODUCTION_STOP_START_MIN)) {

        clMark *blackStop = NULL;
        clMark *blackStart = NULL;

        // check blackscreen before stop/start
        // if direct before logo stop is a blackscreen mark stop/start pair, this logo stop is a valid stop mark
        blackStop = blackMarks->GetPrev(logoStopStartPair->stopPosition + 1, MT_NOBLACKSTOP);  // blackscreen can stop at the same position as logo stop
        blackStart = blackMarks->GetPrev(logoStopStartPair->stopPosition + 1, MT_NOBLACKSTART); // blackscreen can start at the same position as logo stop
        if ( blackStop && blackStart) {
            int diff = 100 * (logoStopStartPair->stopPosition - blackStart->position) / framesPerSecond;
            int lengthBlack = 100 * (blackStart->position - blackStop->position) / framesPerSecond;
            dsyslog("cEvaluateLogoStopStartPair::isInfoLogo():                 ????? stop (%d) start (%d) pair: blacksceen pair before (%d) and (%d) length %dms, diff %dms", logoStopStartPair->stopPosition, logoStopStartPair->startPosition, blackStop->position, blackStart->position, lengthBlack, diff);
            if ((lengthBlack > 200) && (diff < 1)) {
                dsyslog("cEvaluateLogoStopStartPair::isInfoLogo():                 ---- stop (%d) start (%d) pair: blacksceen pair long and near, no info logo part", logoStopStartPair->stopPosition, logoStopStartPair->startPosition);
                logoStopStartPair->isInfoLogo = -1;
                return;
            }
        }
        // check blackscreen between stop/start
        blackStop = blackMarks->GetNext(logoStopStartPair->stopPosition, MT_NOBLACKSTOP);
        blackStart = NULL;
        if (blackStop) blackStart = blackMarks->GetNext(blackStop->position, MT_NOBLACKSTART);
        if ( blackStop && blackStart && (blackStop->position <= logoStopStartPair->startPosition) && (blackStart->position <= logoStopStartPair->startPosition)) {
            int lengthBlack = 100 * (blackStart->position - blackStop->position) / framesPerSecond;
            dsyslog("cEvaluateLogoStopStartPair::isInfoLogo():                 ----- stop (%d) start (%d) pair: in between blacksceen (%d) and (%d) length %dms", logoStopStartPair->stopPosition, logoStopStartPair->startPosition, blackStop->position, blackStart->position, lengthBlack);
            if (lengthBlack > 28) { // changed from 5 to 8 to 28
                logoStopStartPair->isInfoLogo = -1;
                return;
            }
        }
        // check blackscreen after stop/start
        // if direct after logo start is a blackscreen mark stop/start pair, this logo stop is a valid stop mark with closing credits after
        blackStop = blackMarks->GetNext(logoStopStartPair->startPosition - 1, MT_NOBLACKSTOP);  // blackscreen can stop at the same position as logo stop
        blackStart = blackMarks->GetNext(logoStopStartPair->startPosition - 1, MT_NOBLACKSTART); // blackscreen can start at the same position as logo stop
        if ( blackStop && blackStart) {
            int diff = 100 * (blackStart->position - logoStopStartPair->startPosition) / framesPerSecond;
            int lengthBlack = 100 * (blackStop->position - blackStart->position) / framesPerSecond;
            dsyslog("cEvaluateLogoStopStartPair::isInfoLogo():                 ????? stop (%d) start (%d) pair: blacksceen pair after (%d) and (%d) length %dms, diff %dms", logoStopStartPair->stopPosition, logoStopStartPair->startPosition, blackStop->position, blackStart->position, lengthBlack, diff);
            if ((lengthBlack > 2000) && (diff < 1)) {
                dsyslog("cEvaluateLogoStopStartPair::isInfoLogo():                 ---- stop (%d) start (%d) pair: blacksceen pair long and near, no info logo part", logoStopStartPair->stopPosition, logoStopStartPair->startPosition);
                logoStopStartPair->isInfoLogo = -1;
                return;
            }
        }

        dsyslog("cEvaluateLogoStopStartPair::isInfoLogo():                 +++++ stop (%d) start (%d) pair: possible info logo section found, length  %ds (expect <= %ds and >=%ds)", logoStopStartPair->stopPosition, logoStopStartPair->startPosition, length, LOGO_INTRODUCTION_STOP_START_MIN, LOGO_INTRODUCTION_STOP_START_MAX);
        logoStopStartPair->isInfoLogo = 0;
        return;
    }
    else {
        dsyslog("cEvaluateLogoStopStartPair::isInfoLogo():                 ----- stop (%d) start (%d) pair: possible info logo section found, length  %ds (expect <= %ds and >=%ds)", logoStopStartPair->stopPosition, logoStopStartPair->startPosition, length, LOGO_INTRODUCTION_STOP_START_MIN, LOGO_INTRODUCTION_STOP_START_MAX);
    }
    logoStopStartPair->isInfoLogo = -1;
    return;
}


bool cEvaluateLogoStopStartPair::GetNextPair(int *stopPosition, int *startPosition, int *isLogoChange, int *isInfoLogo) {
    if (!stopPosition) return false;
    if (!startPosition) return false;
    if (!isLogoChange) return false;
    if (!isInfoLogo) return false;
    if (nextLogoPairIterator == logoPairVector.end()) return false;

    while ((nextLogoPairIterator->isLogoChange == -1) && (nextLogoPairIterator->isInfoLogo == -1)) {
        ++nextLogoPairIterator;
        if (nextLogoPairIterator == logoPairVector.end()) return false;
    }
    *stopPosition = nextLogoPairIterator->stopPosition;
    *startPosition = nextLogoPairIterator->startPosition;
    *isLogoChange = nextLogoPairIterator->isLogoChange;
    *isInfoLogo = nextLogoPairIterator->isInfoLogo;
    ++nextLogoPairIterator;
    return true;
}
