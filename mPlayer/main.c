//
//  main.c
//  mPlayer
//
//  Created by Rohan Jyoti on 5/16/12.
//  Copyright (c) 2012 __MyCompanyName__. All rights reserved.
//

#include <CoreFoundation/CoreFoundation.h>
#include <AudioToolbox/AudioToolbox.h>

#define defOp "Default errCheck Operation"
#define kNumberPlaybackBuffers 5 //must be at least 3
#define kPlaybackFileLocation CFSTR("/EMPLICIT/zTemp/Archetek-Stay.m4a")

#ifndef mPlayer_h 
#define mPlayer_h
static void errCheck(OSStatus error, const char *operation);
static void mAQOutputCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inCompleteAQBuffer);
static void CalculateBytesForTime(AudioFileID inAudioFile,
								  AudioStreamBasicDescription inDesc,
								  Float64 inSeconds,
								  UInt32 *outBufferSize,
								  UInt32 *outNumPackets);
static void mCopyEncoderCookieToQueue(AudioFileID theFile, AudioQueueRef aQ);
#endif

//===============================================================
//		Features and Limits of Queue Based Playback				|
//																|
// - Allows level metering --> visualization					|
//		kAudioQueueProperty_EnableLevelMetering					|
// - high latency: kNumberPlayback*buffersize(0.5 in this)		|
// - Using AudioUnits for playback offers lower latency			|
// - latency vs cpu (lower latency means more callbacks,		|
//		means more cpu cycles									|
//______________________________________________________________/

 


#pragma mark user info struct
typedef struct mPlay
{
	AudioFileID						playbackFile;
	SInt64							packetPosition;
	UInt32							numPacketsToRead;
	AudioStreamPacketDescription	*packetDescription;
	Boolean							isDone;
}mPlay;

#pragma mark utility functions
static void errCheck(OSStatus error, const char *operation)
{
	if(error == noErr) return;
	
	char errorString[20];
	*(UInt32 *)(errorString + 1) = CFSwapInt32HostToBig(error);
	//see if 4-char code
	if(isprint(errorString[1]) && isprint(errorString[2]) && isprint(errorString[3]) && isprint(errorString[4]))
	{
		errorString[0] = errorString[5] = '\'';
		errorString[6] = '\0';
	}
	else
		sprintf(errorString, "%d", (int)error); //format as integer instead
	
	fprintf(stderr, "Error: %s (%s)\n", operation, errorString);
	exit(1);
}

static void CalculateBytesForTime(AudioFileID inAudioFile,
								  AudioStreamBasicDescription inDesc,
								  Float64 inSeconds,
								  UInt32 *outBufferSize,
								  UInt32 *outNumPackets)
{
	UInt32 maxPacketSize; //we will get the maximum packet size in the file's encoding in the subsequent func. call with that particular 2nd param.
	UInt32 propSize = sizeof(maxPacketSize);
	errCheck(AudioFileGetProperty(inAudioFile, kAudioFilePropertyPacketSizeUpperBound, &propSize, &maxPacketSize), "AudioFileGetProperty error");
	
	static const int maxBufferSize = 0x10000; //65,536 in decimal or 64KB
	static const int minBufferSize = 0x4000; //16,384 in decimal or 16KB
	
	if(inDesc.mFramesPerPacket)
	{
		//the asbd happens to tell us how many frames there are in a packet
		Float64 numPacketsForTime = inDesc.mSampleRate / inDesc.mFramesPerPacket * inSeconds;
		*outBufferSize = numPacketsForTime * maxPacketSize;
	}
	else
	{
		//asbd dosn't tell about number of frames in a packet, we need to arbitrarily pick the max(maxBufferSize, maxPacketSize)
		*outBufferSize = maxBufferSize > maxPacketSize ? maxBufferSize : maxPacketSize;
		//if maxBufferSize > maxPacketSize, then maxBufferSize, else maxPacketSize
	}
	
	if( (*outBufferSize > maxBufferSize) && (*outBufferSize > maxPacketSize) )
		*outBufferSize = maxBufferSize;
	else
	{
		if(*outBufferSize < minBufferSize)
			*outBufferSize = minBufferSize;
	}
	*outNumPackets = *outBufferSize / maxPacketSize;
}

static void mCopyEncoderCookieToQueue(AudioFileID theFile, AudioQueueRef aQ)
{
	UInt32 propertySize;
	OSStatus result = AudioFileGetPropertyInfo(theFile, kAudioFilePropertyMagicCookieData, &propertySize, NULL);
	
	if(result == noErr && propertySize > 0)
	{
		Byte *magicCookie = (UInt8 *) malloc(sizeof(UInt8)*propertySize);
		errCheck(AudioFileGetProperty(theFile, kAudioFilePropertyMagicCookieData, &propertySize, magicCookie), "AudioFileGetProperty get cookie error");
		errCheck(AudioQueueSetProperty(aQ, kAudioQueueProperty_MagicCookie, magicCookie, propertySize), "AudioQueueSetProperty cookie error");
		
		free(magicCookie);
	}
}

#pragma mark playback callback function
static void mAQOutputCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inCompleteAQBuffer)
{
	mPlay *aqp = (mPlay *)inUserData;
	if(aqp->isDone) return; //i.e. the file is done
	
	UInt32 numBytes;
	UInt32 numPackets = aqp->numPacketsToRead;
	errCheck(AudioFileReadPackets(aqp->playbackFile, FALSE, &numBytes, aqp->packetDescription, aqp->packetPosition, &numPackets, inCompleteAQBuffer->mAudioData), "AudioFileReadPackets error");
	
	if(numPackets > 0)
	{
		inCompleteAQBuffer -> mAudioDataByteSize = numBytes;
		AudioQueueEnqueueBuffer(inAQ, inCompleteAQBuffer, (aqp->packetDescription ? numPackets : 0), aqp->packetDescription);
		aqp->packetPosition += numPackets;
	}
	else
	{
		errCheck(AudioQueueStop(inAQ, FALSE), "AudioQueueStop error");
		//the second parameter in AudioQueueStop: inImmediate, determines whether the queue should stop processing immediately, which
		//was fine (true) for the recorder, but not for playback (false) because there still might be data in the queue to process
		aqp->isDone=TRUE;
	}
}

#pragma mark main function
int main (int argc, const char * argv[])
{

	if(argc < 2)
	{
		printf("Usage mPlayer /path/to/file\n");
		return -1;
	}
	mPlay mPlay = {0}; //init everything to 0
	CFStringRef filePath = CFStringCreateWithCString(kCFAllocatorDefault, argv[1], kCFStringEncodingUnicode);	
	CFShowStr(filePath);
	
	//========== Open An Audio File ==========
	CFURLRef mFileURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, kPlaybackFileLocation, kCFURLPOSIXPathStyle, false); //if boolean is true, you could make playlist out of current dir
	errCheck(AudioFileOpenURL(mFileURL, kAudioFileReadPermission, 0, &mPlay.playbackFile), "AudioFileOpenURL error");
	//Note that CFURLRef allocates in the backend. Since we've opened the file as mPlay.playbackFile, we no longer need to mFileURL
	CFRelease(mFileURL);
	
	//========== Set Up Format ==========
	AudioStreamBasicDescription asbdDataFormat;
	UInt32 propSize = sizeof(asbdDataFormat);
	errCheck(AudioFileGetProperty(mPlay.playbackFile, kAudioFilePropertyDataFormat, &propSize, &asbdDataFormat), "AudioFileGetProperty error");
	//now that we have the data format, we now proceed to create the audio queue for playback
	
	//========== Set Up Queue ==========
	AudioQueueRef aQ;
	errCheck(AudioQueueNewOutput(&asbdDataFormat, mAQOutputCallback, &mPlay, NULL, NULL, 0, &aQ), "AudioQueueNewInput error");
	//We have to account for the encoding characterstics of audio: is it compressed or uncompressed? Variable or constant bit rate?
	//Recall that a packet is a collection of frames
		//A frame is a colelction of samples
	//In order to allocate the buffers that the queue will use, we need to inspect the file and its audio encoding to figure out
	//the size of the data buffer and how many packets will be read on each callback.
	UInt32 bufferByteSize;
	CalculateBytesForTime(mPlay.playbackFile, asbdDataFormat, 0.5, &bufferByteSize, &mPlay.numPacketsToRead);
	
	//recall that if the asbd mBytesPerPacket or mFramesPerPacket is 0, then we are dealing with a VBR compressed file
	bool isFormatVBR = (asbdDataFormat.mBytesPerPacket == 0) || (asbdDataFormat.mFramesPerPacket == 0);
	if(isFormatVBR)
		mPlay.packetDescription = (AudioStreamPacketDescription *) malloc(sizeof(AudioStreamPacketDescription) *mPlay.numPacketsToRead);
	else
		mPlay.packetDescription = NULL;
	
	//handle the magic cookie
	mCopyEncoderCookieToQueue(mPlay.playbackFile, aQ);
	
	//========== Start Queue ==========
	AudioQueueBufferRef buffers[kNumberPlaybackBuffers];
	mPlay.isDone = FALSE;
	mPlay.packetPosition = 0;
	int i;
	for(i=0; i<kNumberPlaybackBuffers; i++)
	{
		errCheck(AudioQueueAllocateBuffer(aQ, bufferByteSize, &buffers[i]), "AudioQueueAllocateBuffers error");
		mAQOutputCallback(&mPlay, aQ, buffers[i]);
		
		if(mPlay.isDone) break; //note that mPlay (and consequently isDone) gets updated with the callback above
	}
	
	errCheck(AudioQueueStart(aQ, NULL), "AudioQueueStart error");
	printf("File Playing...\n");
	
	while(!mPlay.isDone)
	{
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
	}
	//to make sure every buffer gets played, play for an additional kNumberPlaybackBuffer * callback time
	int tailTime = kNumberPlaybackBuffers * 0.5;
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, tailTime, false);
	
	//========== Stop Queue / Clean Up ==========
	mPlay.isDone = TRUE;
	errCheck(AudioQueueStop(aQ, TRUE), "AudioQueueStop error");
	AudioQueueDispose(aQ, TRUE);
	AudioFileClose(mPlay.playbackFile);
    return 0;
}



