/*
 *  SinkManager.cpp - Class that handles multiple server sessions dynamically.
 *  Copyright (C) 2014  Fundació i2CAT, Internet i Innovació digital a Catalunya
 *
 *  This file is part of liveMediaStreamer.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Authors:  David Cassany <david.cassany@i2cat.net>,
 *            
 */


#include <GroupsockHelper.hh>

#include "SinkManager.hh"
#include "../../AVFramedQueue.hh"
#include "../../AudioCircularBuffer.hh"
#include "H264QueueServerMediaSubsession.hh"
#include "VP8QueueServerMediaSubsession.hh"
#include "AudioQueueServerMediaSubsession.hh"
#include "QueueSource.hh"
#include "H264QueueSource.hh"


SinkManager *SinkManager::mngrInstance = NULL;

SinkManager::SinkManager(int readersNum): TailFilter(readersNum), watch(0)
{    
    TaskScheduler* scheduler = BasicTaskScheduler::createNew();
    this->env = BasicUsageEnvironment::createNew(*scheduler);
    
    //TODO: Add authentication security
    rtspServer = RTSPServer::createNew(*env, RTSP_PORT, NULL);
    if (rtspServer == NULL) {
        utils::errorMsg("Failed to create RTSP server");
    }
    
    OutPacketBuffer::increaseMaxSizeTo(MAX_VIDEO_FRAME_SIZE);
    
    mngrInstance = this;
    fType = TRANSMITTER;
    initializeEventMap();
}

SinkManager::~SinkManager()
{
    delete &envir()->taskScheduler();
    envir()->reclaim();
    env = NULL;
    
    mngrInstance = NULL;
}

SinkManager* 
SinkManager::getInstance(){
    if (mngrInstance != NULL){
        return mngrInstance;
    }
    return new SinkManager();
}

void SinkManager::stop()
{
    for (auto it : sessionList ) {
        rtspServer->deleteServerMediaSession(it.second);
    }
    
    for (auto it : replicas) {
       Medium::close(it.second);
    }
    
    for (auto it : connections) {
       delete it.second;
    }

    Medium::close(rtspServer);

    watch = 1;
}

void SinkManager::destroyInstance()
{
    //TODO:
}

bool SinkManager::processFrame(bool removeFrame)
{   
    if (envir() == NULL){
        return false;
    }
    
    envir()->taskScheduler().doEventLoop((char*) &watch);

    return true;
}

bool SinkManager::addSession(std::string id, std::vector<int> readers, std::string info, std::string desc)
{
    if (!rtspServer){
        utils::errorMsg("Unitialized RTSPServer");
        return false;
    }

    if (sessionList.count(id) > 0){
        utils::errorMsg("Failed, this session already exists: " + id);
        return false;
    }
    
    ServerMediaSession* servSession
        = ServerMediaSession::createNew(*(envir()), id.c_str(), info.c_str(), desc.c_str());
        
    if (servSession == NULL){
        utils::errorMsg("Failed creating new ServerMediaSession");
        return false;
    }
    
    ServerMediaSubsession *subsession;
    
    for (auto & reader : readers){
        if ((subsession = createSubsessionByReader(reader)) != NULL) {
            servSession->addSubsession(subsession);
        } else {
            servSession->deleteAllSubsessions();
            Medium::close(servSession);
            return false;
        }
    }
    
    sessionList[id] = servSession;
    
    return true;
}

bool SinkManager::addConnection(int reader, unsigned id, std::string ip, unsigned int port, TxFormat txFmt)
{
    bool success = false;

    if (connections.count(id) > 0){
        utils::errorMsg("Connection id must be unique");
        return success;
    }

    switch(txFmt) {
        case RAW:
            success = addRawConnection(reader, id, ip, port);
        break;
        case ULTRAGRID:
            success = addUltraGridConnection(reader, id , ip ,port)
        break;
        case MPEGTS:
            // NOTE: in this case more than one reader can be attached to MPEGTS because it muxes the data.
            // Think about this
        break;
        default:
            utils::errorMsg("Error creating connections. Transport format not supported");
        break;
    }
}

bool SinkManager::addRawConnection(int reader, unsigned id, std::string ip, unsigned int port)
{
    VideoFrameQueue *vQueue;
    AudioFrameQueue *aQueue;

    if (connections.count(id) > 0){
        utils::errorMsg("Connection id must be unique");
        return false;
    }

    if ((vQueue = dynamic_cast<VideoFrameQueue*>(getReader(reader)->getQueue())) != NULL){
        connections[id] = new VideoConnection(envir(), ip, port, 
                                              replicas[reader]->createStreamReplica(), 
                                              vQueue->getCodec());
        return true;
    }
    if ((aQueue = dynamic_cast<AudioFrameQueue*>(getReader(reader)->getQueue())) != NULL){ 
        connections[id] = new AudioConnection(envir(), ip, port, 
                                              replicas[reader]->createStreamReplica(), 
                                              aQueue->getCodec(), aQueue->getChannels(),
                                              aQueue->getSampleRate(), aQueue->getSampleFmt());
        return true;
    }
    return false;
}

bool SinkManager::addUltraGridConnection(int reader, unsigned id, std::string ip, unsigned int port)
{
    
}

bool SinkManager::addDashConnection(int reader, unsigned id, std::string fileName, std::string quality, bool reInit, uint32_t segmentTime, uint32_t initSegment, uint32_t fps)
{
    VideoFrameQueue *vQueue;
    AudioFrameQueue *aQueue;

    if (connections.count(id) > 0){
        utils::errorMsg("Connection id must be unique");
        return false;
    }
    if ((vQueue = dynamic_cast<VideoFrameQueue*>(getReader(reader)->getQueue())) != NULL){
        connections[id] = new DashVideoConnection(envir(), fileName, 
                                              replicas[reader]->createStreamReplica(), 
                                              vQueue->getCodec(), quality, fps, reInit, segmentTime, initSegment);
        return true;
    }
    if ((aQueue = dynamic_cast<AudioFrameQueue*>(getReader(reader)->getQueue())) != NULL){ 
        connections[id] = new DashAudioConnection(envir(), fileName, 
                                              replicas[reader]->createStreamReplica(), 
                                              aQueue->getCodec(), aQueue->getChannels(),
                                              aQueue->getSampleRate(), aQueue->getSampleFmt(), quality, reInit, segmentTime, initSegment);
        return true;
    }
    return false;
}

Reader *SinkManager::setReader(int readerId, FrameQueue* queue, bool sharedQueue)
{
    VideoFrameQueue *vQueue;
    AudioFrameQueue *aQueue;
    
    if (readers.size() >= getMaxReaders() || readers.count(readerId) > 0 ) {
        return NULL;
    }
    
    Reader* r = new Reader();
    readers[readerId] = r;
    
    if ((vQueue = dynamic_cast<VideoFrameQueue*>(queue)) != NULL){
        createVideoQueueSource(vQueue->getCodec(), r, readerId);
    }
    
    if ((aQueue = dynamic_cast<AudioFrameQueue*>(queue)) != NULL){
        createAudioQueueSource(aQueue->getCodec(), r, readerId);
    }
    
    return r;
}

void SinkManager::createVideoQueueSource(VCodecType codec, Reader *reader, int readerId)
{
    FramedSource *source;
    switch(codec){
        case H264:
            source = H264QueueSource::createNew(*(envir()), reader, readerId);
            replicas[readerId] = StreamReplicator::createNew(*(envir()), source, False);
            break;
        case VP8:
        default:
            source = QueueSource::createNew(*(envir()), reader, readerId);
            replicas[readerId] =  StreamReplicator::createNew(*(envir()), source, False);
            break;
    }
}

void SinkManager::createAudioQueueSource(ACodecType codec, Reader *reader, int readerId)
{
    QueueSource *source;
    switch(codec){
       // case MPEG4_GENERIC:
			//printf("TODO createAudioQueueSource\n");
            //TODO
            //break;
        default:
            source = QueueSource::createNew(*(envir()), reader, readerId);
            replicas[readerId] = StreamReplicator::createNew(*(envir()), source, False);
            break;
    }
}

ServerMediaSubsession *SinkManager::createVideoMediaSubsession(VCodecType codec, int readerId)
{
    switch(codec){
        case H264:
            return H264QueueServerMediaSubsession::createNew(*(envir()), replicas[readerId], readerId, False);
            break;
        case VP8:
            return VP8QueueServerMediaSubsession::createNew(*(envir()), replicas[readerId], readerId, False);
            break;
        default:
            break;
    }
    return NULL;
}

ServerMediaSubsession *SinkManager::createAudioMediaSubsession(ACodecType codec,
                                                               unsigned channels,
                                                               unsigned sampleRate,
                                                               SampleFmt sampleFormat, 
                                                               int readerId)
{
    switch(codec){
        //case MPEG4_GENERIC:
            //TODO
			//printf("TODO createAudioMediaSubsession\n");
            //break;
        default:
            return AudioQueueServerMediaSubsession::createNew(*(envir()), replicas[readerId], 
                                                              readerId, codec, channels,
                                                              sampleRate, sampleFormat, False);
            break;
    }
    return NULL;
}

ServerMediaSubsession *SinkManager::createSubsessionByReader(int readerId)
{
    VideoFrameQueue *vQueue;
    AudioFrameQueue *aQueue;
    
    if ((vQueue = dynamic_cast<VideoFrameQueue*>(getReader(readerId)->getQueue())) != NULL){
        return createVideoMediaSubsession(vQueue->getCodec(), readerId);
    }

    if ((aQueue = dynamic_cast<AudioFrameQueue*>(getReader(readerId)->getQueue())) != NULL){
        return createAudioMediaSubsession(aQueue->getCodec(), aQueue->getChannels(),
                                          aQueue->getSampleRate(), aQueue->getSampleFmt(), readerId);
    }

    return NULL;
}

bool SinkManager::removeSession(std::string id)
{   
    if (sessionList.count(id) <= 0){
        utils::errorMsg("Failed, no session found with this id (" + id + ")");
        return false;
    }

    rtspServer->deleteServerMediaSession(sessionList[id]);
    sessionList.erase(id);
    
    return false;
}

bool SinkManager::removeSessionByReaderId(int readerId)
{
    bool ret = false;
    QueueServerMediaSubsession *qSubsession;
    ServerMediaSubsession *subsession;
    
    std::map<std::string, ServerMediaSession*>::iterator it = sessionList.begin();
    while(it != sessionList.end()){
        ServerMediaSubsessionIterator subIt(*it->second);
        subsession = subIt.next();
        while(subsession){
            if (!(qSubsession = dynamic_cast<QueueServerMediaSubsession *>(subsession))){
                utils::errorMsg("Could not cast to QueueServerMediaSubsession");
                return false;
            }
            if (qSubsession->getReaderId() == readerId){
                std::map<std::string, ServerMediaSession*>::iterator tmpIt = it;
                removeSession(tmpIt->first);
                ret = true;
                break;
            }
            subsession = subIt.next();
        }
        it++;
    }
    return ret;
}

bool SinkManager::deleteReader(int id)
{
    Reader* reader = getReader(id);
    if (reader == NULL){
        return false;
    }
    
    removeSessionByReaderId(id);
    
    if (reader->isConnected()) {
        return false;
    }
    
    delete reader;
    readers.erase(id);
    
    return true;
}

bool SinkManager::publishSession(std::string id)
{  
    if (sessionList.find(id) == sessionList.end()){
        utils::errorMsg("Failed, no session found with this id (" + id + ")");
        return false;
    }
    
    if (rtspServer == NULL){
        return false;
    }
    
    rtspServer->addServerMediaSession(sessionList[id]);
    char* url = rtspServer->rtspURL(sessionList[id]);
    
    utils::infoMsg("Play " + id + " stream using the URL " + url);
    delete[] url;
    
    return true;
}

ServerMediaSession* SinkManager::getSession(std::string id)
{
    if(sessionList.find(id) != sessionList.end()){
        return sessionList[id];
    }
    
    return NULL;
}

void SinkManager::initializeEventMap() 
{
     eventMap["addSession"] = std::bind(&SinkManager::addSessionEvent, this, 
                                        std::placeholders::_1,  std::placeholders::_2);
}

void SinkManager::addSessionEvent(Jzon::Node* params, Jzon::Object &outputNode)
{
    std::vector<int> readers;
    std::string sessionId;

    if (!params) {
        outputNode.Add("error", "Error adding session. No parameters!");
        return;
    }

    if (params->Has("sessionName")) {
        sessionId = params->Get("sessionName").ToString();
    } else {
        sessionId = utils::randomIdGenerator(ID_LENGTH);
    }

    if (!params->Has("readers") || !params->Get("readers").IsArray()) {
        outputNode.Add("error", "Error adding session. Readers does not exist or is not an array!");
        return;
    }
        
    Jzon::Array jsonReaders = params->Get("readers").AsArray();
    
    for (Jzon::Array::iterator it = jsonReaders.begin(); it != jsonReaders.end(); ++it) {
        readers.push_back((*it).ToInt());
    }
    
    if (readers.empty()) {
        outputNode.Add("error", "Error adding session. Readers array is empty!");
        return;
    }

    if(!addSession(sessionId, readers)) {
        outputNode.Add("error", "Error adding session. Internal error!");
        return;
    }

    if (!publishSession(sessionId)){
        outputNode.Add("error", "Error adding session. Internal error!");
        return;
    }

    outputNode.Add("sessionID", sessionId);
} 

void SinkManager::doGetState(Jzon::Object &filterNode)
{
    Jzon::Array sessionArray;
    ServerMediaSubsession* subsession;
    int readerId;

    for (auto it : sessionList) {
        Jzon::Array jsonReaders;
        Jzon::Object jsonSession;
        std::string uri = rtspServer->rtspURL(it.second);

        jsonSession.Add("id", it.first);
        jsonSession.Add("uri", uri);

        ServerMediaSubsessionIterator sIt(*it.second);
        subsession = sIt.next();

        while(subsession) {
            readerId = dynamic_cast<QueueServerMediaSubsession*>(subsession)->getReaderId();
            jsonReaders.Add(readerId);
            subsession = sIt.next();
        }

        jsonSession.Add("readers", jsonReaders);
        sessionArray.Add(jsonSession);
    }

    filterNode.Add("sessions", sessionArray);
}

Connection::Connection(UsageEnvironment* env, 
                       std::string ip, unsigned port, FramedSource *source) : 
                       fEnv(env), fIp(ip), fPort(port), fSource(source)
{
    unsigned serverPort = INITIAL_SERVER_PORT;
    destinationAddress.s_addr = our_inet_addr(fIp.c_str());
    if ((port%2) != 0){
        utils::warningMsg("Port should be an even number");
    }
    const Port rtpPort(port);
    const Port rtcpPort(port+1);
    
    for (;;serverPort+=2){
        rtpGroupsock = new Groupsock(*fEnv, destinationAddress, Port(serverPort), TTL);
        rtcpGroupsock = new Groupsock(*fEnv, destinationAddress, Port(serverPort+1), TTL);
        if (rtpGroupsock->socketNum() < 0 || rtcpGroupsock->socketNum() < 0) {
            delete rtpGroupsock;
            delete rtcpGroupsock;
            continue; // try again
        }
        break;
    }
    
    rtpGroupsock->changeDestinationParameters(destinationAddress, rtpPort, TTL);
    rtcpGroupsock->changeDestinationParameters(destinationAddress, rtcpPort, TTL);
}

Connection::Connection(UsageEnvironment* env, 
                       std::string fileName, FramedSource *source) : 
                       fEnv(env), fFileName(fileName), fSource(source)
{
	
}

Connection::~Connection()
{
    Medium::close(fSource);
    if (sink) {
        sink->stopPlaying();
        Medium::close(sink);
        Medium::close(rtcp);
    }
    delete rtpGroupsock;
    delete rtcpGroupsock;
}

void Connection::afterPlaying(void* clientData) {
    RTPSink *clientSink = (RTPSink*) clientData;
    clientSink->stopPlaying();
}

void Connection::startPlaying()
{
    if (sink != NULL){
        
        sink->startPlaying(*fSource, &Connection::afterPlaying, sink);
    }
    if (outputVideoFile != NULL){
        
        outputVideoFile->startPlaying(*fSource, &Connection::afterPlaying, NULL);
    }
}

void Connection::stopPlaying()
{
    if (sink != NULL){
        sink->stopPlaying();
    }
    if (outputVideoFile != NULL){
        outputVideoFile->stopPlaying();
    }
}

VideoConnection::VideoConnection(UsageEnvironment* env, 
                                 std::string ip, unsigned port, 
                                 FramedSource *source, VCodecType codec) : 
                                 Connection(env, ip, port, source), fCodec(codec)
{
    switch(fCodec){
        case H264:
            sink = H264VideoRTPSink::createNew(*fEnv, rtpGroupsock, 96);
            fSource = H264VideoStreamDiscreteFramer::createNew(*fEnv, fSource);
            break;
        case VP8:
            sink = VP8VideoRTPSink::createNew(*fEnv, rtpGroupsock, 96);
            break;
        default:
            sink = NULL;
            break;
    }
    if (sink != NULL){
        const unsigned maxCNAMElen = 100;
        unsigned char CNAME[maxCNAMElen+1];
        gethostname((char*)CNAME, maxCNAMElen);
        CNAME[maxCNAMElen] = '\0';
        rtcp = RTCPInstance::createNew(*fEnv, rtcpGroupsock, 5000, CNAME,
                                       sink, NULL, False);
    } else {
        utils::errorMsg("VideoConnection could not be created");
    }
    startPlaying();
}

DashVideoConnection::DashVideoConnection(UsageEnvironment* env, 
                                 std::string fileName, 
                                 FramedSource *source, VCodecType codec, std::string quality, uint32_t fps, bool reInit, uint32_t segmentTime, uint32_t initSegment) : 
                                 Connection(env, fileName, source), fCodec(codec), fReInit(reInit), fFps(fps), fSegmentTime(segmentTime), fInitSegment(initSegment)
{
    switch(fCodec){
        case H264:
            outputVideoFile = DashFileSink::createNew(*env, fileName.c_str(), MAX_DAT, True, quality.c_str(), fInitSegment, "m4v", ONLY_VIDEO, false);
            fSource = DashSegmenterVideoSource::createNew(*fEnv, source, fReInit, fFps, fSegmentTime);
            break;
        default:
            sink = NULL;
            break;
    }
   if (outputVideoFile != NULL){
		//TODO??
    } else {
        utils::errorMsg("VideoConnection could not be created");
    }
    startPlaying();
}

AudioConnection::AudioConnection(UsageEnvironment* env, 
                                 std::string ip, unsigned port, 
                                 FramedSource *source, ACodecType codec, 
                                 unsigned channels, unsigned sampleRate, 
                                 SampleFmt sampleFormat) : Connection(env, ip, port, source), 
                                 fCodec(codec), fChannels(channels), fSampleRate(sampleRate), 
                                 fSampleFormat(sampleFormat)
{
    unsigned char payloadType = 97;
    std::string codecStr = utils::getAudioCodecAsString(fCodec);
    
    if (fCodec == PCM && 
        fSampleFormat == S16) {
        codecStr = "L16";
        if (fSampleRate == 44100) {
            if (fChannels == 2){
                payloadType = 10;
            } else if (fChannels == 1) {
                payloadType = 11;
            }
        }
    } else if ((fCodec == PCMU || fCodec == G711) && 
                fSampleRate == 8000 &&
                fChannels == 1){
        payloadType = 0;
    }
    
    if (fCodec == MP3){
        sink =  MPEG1or2AudioRTPSink::createNew(*fEnv, rtpGroupsock);
    } else {
        sink =  SimpleRTPSink
            ::createNew(*fEnv, rtpGroupsock, payloadType,
                        fSampleRate, "audio", 
                        codecStr.c_str(),
                        fChannels, False);
    }
    
    if (sink != NULL){
        const unsigned maxCNAMElen = 100;
        unsigned char CNAME[maxCNAMElen+1];
        gethostname((char*)CNAME, maxCNAMElen);
        CNAME[maxCNAMElen] = '\0';
        rtcp = RTCPInstance::createNew(*fEnv, rtcpGroupsock, 5000, CNAME,
                                       sink, NULL, False);
    } else {
        utils::errorMsg("AudioConnection could not be created");
    }
    startPlaying();
}

DashAudioConnection::DashAudioConnection(UsageEnvironment* env, 
                                 std::string fileName, 
                                 FramedSource *source, ACodecType codec, 
                                 unsigned channels, unsigned sampleRate, 
                                 SampleFmt sampleFormat, std::string quality, bool reInit, uint32_t segmentTime, uint32_t initSegment) : Connection(env, fileName, source), 
                                 fCodec(codec), fChannels(channels), fSampleRate(sampleRate), 
                                 fSampleFormat(sampleFormat), fReInit(reInit), fSegmentTime(segmentTime), fInitSegment(initSegment)
{
	switch (fCodec) {
		case MPEG4_GENERIC:
            outputVideoFile = DashFileSink::createNew(*env, fileName.c_str(), MAX_DAT, True, quality.c_str(), fInitSegment, "m4a", ONLY_AUDIO, false);
            fSource = DashSegmenterAudioSource::createNew(*fEnv, source, fReInit, fSegmentTime, fSampleRate);
            break;
        default:
            sink = NULL;
            break;
    }
   if (outputVideoFile != NULL){
		//TODO??
    } else {
        utils::errorMsg("AudioConnection could not be created");
    }
    startPlaying();    
}

