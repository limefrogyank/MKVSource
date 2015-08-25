//////////////////////////////////////////////////////////////////////////
//
// Parse.h
// MPEG-1 parsing code.
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
//////////////////////////////////////////////////////////////////////////

#pragma once


// Note: The structs, enums, and constants defined in this header are not taken from
// Media Foundation or DirectShow headers. The parser code is written to be API-agnostic.
// The only exceptions are:
//    - Use of the MFRatio structure to describe ratios.
//    - The MPEG1AudioFlags enum defined here maps directly to the equivalent DirectShow flags.

// Sizes
const DWORD MPEG1_MAX_PACKET_SIZE = 65535 + 6;          // Maximum packet size.
const DWORD MPEG1_PACK_HEADER_SIZE = 12;                // Pack header.

const DWORD MPEG1_SYSTEM_HEADER_MIN_SIZE = 12;          // System header, excluding the stream info.
const DWORD MPEG1_SYSTEM_HEADER_PREFIX = 6;             // This value + header length = total size of the system header.
const DWORD MPEG1_SYSTEM_HEADER_STREAM = 3;             // Size of each stream info in the system header.

const DWORD MPEG1_PACKET_HEADER_MIN_SIZE = 6;           // Minimum amount to read in the packet header. (Up to the variable-sized padding bytes)
const DWORD MPEG1_PACKET_HEADER_MAX_STUFFING_BYTE = 16; // Maximum number of stuffing bytes in a packet header.
const DWORD MPEG1_PACKET_HEADER_MAX_SIZE = 34;          // Maximum size of a packet header.

const DWORD MPEG1_VIDEO_SEQ_HEADER_MIN_SIZE = 12;       // Minimum length of the video sequence header.
const DWORD MPEG1_VIDEO_SEQ_HEADER_MAX_SIZE = 140;      // Maximum length of the video sequence header.

const DWORD MPEG1_AUDIO_FRAME_HEADER_SIZE = 4;


// Codes
const DWORD MPEG1_START_CODE_PREFIX = 0x00000100;
const DWORD MPEG1_PACK_START_CODE = 0x000001BA;
const DWORD MPEG1_SYSTEM_HEADER_CODE = 0x000001BB;
const DWORD MPEG1_SEQUENCE_HEADER_CODE = 0x000001B3;
const DWORD MPEG1_STOP_CODE = 0x000001B9;

// Stream ID codes
const BYTE MPEG1_STREAMTYPE_ALL_AUDIO = 0xB8;
const BYTE MPEG1_STREAMTYPE_ALL_VIDEO = 0xB9;
const BYTE MPEG1_STREAMTYPE_RESERVED = 0xBC;
const BYTE MPEG1_STREAMTYPE_PRIVATE1 = 0xBD;
const BYTE MPEG1_STREAMTYPE_PADDING = 0xBE;
const BYTE MPEG1_STREAMTYPE_PRIVATE2 = 0xBF;
const BYTE MPEG1_STREAMTYPE_AUDIO_MASK = 0xC0;
const BYTE MPEG1_STREAMTYPE_VIDEO_MASK = 0xE0;
const BYTE MPEG1_STREAMTYPE_DATA_MASK = 0xF0;



// Systems layer

enum StreamType
{
	StreamType_Unknown,
	StreamType_AllAudio,
	StreamType_AllVideo,
	StreamType_Reserved,
	StreamType_Private1,
	StreamType_Padding,
	StreamType_Private2,
	StreamType_Audio,   // ISO/IEC 11172-3
	StreamType_Video,   // ISO/IEC 11172-2
	StreamType_Data
};

struct MPEG1StreamHeader
{
	BYTE        stream_id;  // Raw stream_id field.
	StreamType  type;       // Stream type (audio, video, etc)
	BYTE        number;     // Index within the stream type (audio 0, audio 1, etc)
	DWORD       sizeBound;
};

// MPEG1SystemHeader
// Holds information from the system header. This structure is variable
// length, because the last field is an array of stream headers.
struct MPEG1SystemHeader
{
	DWORD   cbSize;     // Size of this structure, including the streams array.
	DWORD   rateBound;
	BYTE    cAudioBound;
	bool    bFixed;
	bool    bCSPS;
	bool    bAudioLock;
	bool    bVideoLock;
	BYTE    cVideoBound;
	DWORD   cStreams;
	MPEG1StreamHeader streams[1];   // Array of 1 or more stream headers.
};


struct Video
{
	bool		FlagInterlaced;
	DWORD		StereoMode;
	DWORD		AlphaMode;
	DWORD		PixelWidth;
	DWORD		PixelHeight;
	DWORD		PixelCropBottom;
	DWORD		PixelCropTop;
	DWORD		PixelCropLeft;
	DWORD		PixelCropRight;
	DWORD		DisplayWidth;
	DWORD		DisplayHeight;
	DWORD		DisplayUnit;
	DWORD		AspectRatioType;
	DWORD		ColourSpace;
};

struct Audio
{
	DWORD		SamplingFrequency;
	DWORD		OutputSamplingFrequency;
	BYTE		Channels;
	BYTE		BitDepth;
};

struct TrackPlane
{
	LONG64	TrackPlaneUID;
	byte	TrackPlaneType;
};

struct TrackJoinBlock
{
	LONG64	TackJoinUID;
};

struct TrackOperation
{
	TrackPlane			TrackCombinePlanes[1];
	TrackJoinBlock		TrackJoinBlocks[1];
};

struct ContentCompression
{
	byte		ContentCompAlgo;
	byte		ContentCompSettings[32];
};

struct ContentEncryption
{
	byte		ContentEncAlgo;
	//FINISH LATER
};

struct ContentEncoding
{
	DWORD				ContentEncodingOrder;
	DWORD				ContentEncodingScope;
	DWORD				ContentEncodingType;
	ContentCompression	ContentCompression;
	ContentEncryption	ContentEncryption;
};

struct TrackTranslate
{
	LONG64		TrackTranslateEditionUID;
	DWORD		TrackTracnslateCodec;
	DWORD		TrackTranslateTrackID;
};

struct TrackData
{
	DWORD	TrackNumber;
	LONG64	TrackUID;
	DWORD	TrackType;
	bool	FlagEnabled;
	bool	FlagDefault;
	bool	FlagForced;
	bool	FlagLacing;
	DWORD	MinCache;
	DWORD	MaxCache;
	DWORD	DefaultDuration;
	DWORD	DefaultDecodedFieldDuration;
	DWORD	MaxBlockAdditionID;
	char	Name[32];
	const char*	CodecID;
	byte*	CodecPrivate;
	int		CodecPrivateLength;
	char	CodecName[32];
	LONG64	AttachmentLink;
	bool	CodecDecodeAll;
	DWORD	TrackOverlay;
	DWORD	CodecDelay;
	DWORD	SeekPreRoll;
	TrackTranslate	trackTranslate[1];
	Video*	Video;
	Audio*	Audio;
	TrackOperation trackOperation;
	ContentEncoding ContentEncodings[1];
};


struct Seek
{
	const char*			elemID;
	DWORD64			SeekPosition;

	/*Seek(byte* iD, DWORD64 seekPosition)
		: SeekPosition(SeekPosition)
	{
		memcpy(&ID[0], iD, 4);
	}*/
};

struct SegmentInformation
{
	byte						SegmentUID[16];
	UINT64						TimecodeScale;
	double						Duration;
	const char*					MuxingApp;
	const char*					WritingApp;
};

struct CueTrackPosition
{
	UINT64						CueTrack;
	UINT64						CueClusterPosition;
};

struct CuePoint
{
	UINT64							CueTime;
	std::vector<CueTrackPosition*>	CueTrackPositions;
};

struct MKVMasterData
{
	LONG64						SegmentPosition;
	std::vector<Seek*>			SeekHead;
	SegmentInformation*			SegInfo;
	std::vector<TrackData*>		Tracks;
	LONG64						FirstClusterPosition;
	std::vector<CuePoint*>		Cues;
};




struct MPEG1PacketHeader
{
	BYTE        stream_id;      // Raw stream_id field.
	StreamType  type;           // Stream type (audio, video, etc)
	BYTE        number;         // Index within the stream type (audio 0, audio 1, etc)
	DWORD       cbPacketSize;   // Size of the entire packet (header + payload).
	DWORD       cbPayload;      // Size of the packet payload (packet size - header size).
	bool        bHasPTS;        // Did the packet header contain a Presentation Time Stamp (PTS)?
	LONGLONG    PTS;            // Presentation Time Stamp (in 90 kHz clock)
};

// Video

struct MPEG1VideoSeqHeader
{
	WORD        width;
	WORD        height;
	MFRatio     pixelAspectRatio;
	MFRatio     frameRate;
	DWORD       bitRate;
	WORD        cbVBV_Buffer;
	bool        bConstrained;
	DWORD       cbHeader;
	BYTE        header[MPEG1_VIDEO_SEQ_HEADER_MAX_SIZE];    // Raw header.
};

// Audio

enum MPEG1AudioLayer
{
	MPEG1_Audio_Layer1 = 0,
	MPEG1_Audio_Layer2,
	MPEG1_Audio_Layer3
};

enum MPEG1AudioMode
{
	MPEG1_Audio_Stereo = 0,
	MPEG1_Audio_JointStereo,
	MPEG1_Audio_DualChannel,
	MPEG1_Audio_SingleChannel
};


// Various bit flags used in the audio frame header.
// (Note: These enum values are not the actual values in the audio frame header.)
enum MPEG1AudioFlags
{
	MPEG1_AUDIO_PRIVATE_BIT = 0x01,  // = ACM_MPEG_PRIVATEBIT
	MPEG1_AUDIO_COPYRIGHT_BIT = 0x02,  // = ACM_MPEG_COPYRIGHT
	MPEG1_AUDIO_ORIGINAL_BIT = 0x04,  // = ACM_MPEG_ORIGINALHOME
	MPEG1_AUDIO_PROTECTION_BIT = 0x08,  // = ACM_MPEG_PROTECTIONBIT
};

struct  MPEG1AudioFrameHeader
{
	MPEG1AudioLayer     layer;
	DWORD               dwBitRate;        // Bit rate in Kbits / sec
	DWORD               dwSamplesPerSec;
	WORD                nBlockAlign;
	WORD                nChannels;
	MPEG1AudioMode      mode;
	BYTE                modeExtension;
	BYTE                emphasis;
	WORD                wFlags;    // bitwise OR of MPEG1AudioFlags
};

enum EET
{
	_VOID = 0,
	MASTER = 1,
	_UNSIGNED = 3,
	_SIGNED = 3,
	TEXTA = 4,
	TEXTU = 5,
	BINARY = 6,
	_FLOAT = 7,
	_DATE = 8,
	JUST_GO_ON = 10
};


struct type_name
{
	type_name() : type(), name()
	{ }

	type_name(EET eet, const char* st)
		: type(eet), name(st)
	{
		//std::string str(st);
		//sprintf_s(this->name, 30, "%.30s", str.c_str());
		//strncpy(name, str.c_str(), 30);
		//str.copy(name);
	}

	EET						type;
	const char*					name;
	//char				name[30];
	
	
};


struct type_data
{
	type_data() : type(), data(), size() {}
	type_data(EET eet, void* data, DWORD size)
		: type(eet),
		data(data),
		size(size)
	{ }

	EET						type;
	void*					data;
	DWORD					size;
};

struct cmp_str
{
	bool operator()(char const *a, char const *b)
	{
		return std::strcmp(a, b) < 0;
	}
};


struct base_element
{
	const char*	name;
	EET		type;

	virtual ~base_element()
	{
		//delete name;  don't delete this... a constant from our lookup table
	}
};

struct master_element : base_element
{
	std::vector<base_element*> children;

	~master_element()
	{
		for (int i = 0; i < children.size(); ++i)
		{
			delete children[i];
		}
	}
};

struct binary_element : base_element
{
	DWORD	length;
	byte*	data;

	~binary_element()
	{
		delete data;
	}
};

struct string_element : base_element
{
	const char*		data;

	~string_element()
	{
		delete data;
	}
};

struct sint_element :base_element
{
	INT64		data;

	~sint_element()
	{
		delete &data;
	}
};

struct uint_element :base_element
{
	LONG64		data;

	~uint_element()
	{
		delete &data;
	}
};

struct float_element :base_element
{
	double		data;

	~float_element()
	{
		delete &data;
	}
};



//struct child_element
//{
//	char			name[30];
//	type_data		typedata;
//
//	child_element() :name(), typedata()
//	{ }
//
//	child_element(const char* name, type_data typedata)
//		: typedata(typedata)
//	{
//		std::string str(name);
//		sprintf_s(this->name, 30, "%.30s", str.c_str());
//	}
//
//};



//struct children
//{
//	int					count;
//	child_element*		elements;
//
//	children(int count, child_element* elements)
//		: count(count), elements(elements)
//	{ }
//};

struct num_pos
{
	DWORD			num;
	uint8			pos;

	num_pos(DWORD num, uint8 pos)
		: num(num),
		pos(pos)
	{ }
};

struct bit_number_result
{
	bit_number_result(uint8 bitNum, uint8 clearedNum)
		: bitNum(bitNum),
		clearedNum(clearedNum)
	{ }

	uint8				bitNum;
	uint8				clearedNum;
};

struct matroska_number_result
{
	DWORD				id;
	DWORD				length;
};

struct element_header_result
{
	DWORD				id;
	DWORD				elemsize;
	DWORD				headsize;
};

//template<class T> class Tree {
//class Tree {
//public:
//	child_element node;
//	std::vector<Tree*>* children;
//	// Class interface
//private:
//	
//};


// ExpandableStruct class:
// Class which wraps a structure which can be expanded by additional data.
template<typename T>
ref class ExpandableStruct sealed
{
internal:
	ExpandableStruct(unsigned int size)
		: m_array(ref new Array<BYTE>(size))
	{
		ZeroMemory(m_array->Data, m_array->Length);
	}

	ExpandableStruct(const ExpandableStruct<T> ^src)
		: m_array(ref new Array<BYTE>(src->Size))
	{
		CopyFrom(src);
	}

	property unsigned int Size {unsigned int get() const { return m_array->Length; }}
	T *Get() const { return reinterpret_cast<T*>(m_array->Data); }
	void CopyFrom(const ExpandableStruct<T> ^src)
	{
		if (src->Size != Size)
		{
			throw ref new InvalidArgumentException();
		}

		CopyMemory(Get(), src->Get(), Size);
	}

private:
	Array<BYTE> ^m_array;
};


// Buffer class:
// Resizable buffer used to hold the MPEG-1 data.

ref class Buffer sealed
{
internal:
	Buffer(DWORD cbSize);
	HRESULT Initalize(DWORD cbSize);

	property BYTE *DataPtr { BYTE *get(); }
	property DWORD DataSize { DWORD get() const; }

	// Reserve: Reserves cb bytes of free data in the buffer.
	// The reserved bytes start at DataPtr() + DataSize().
	void Reserve(DWORD cb);

	// MoveStart: Moves the front of the buffer.
	// Call this method after consuming data from the buffer.
	void MoveStart(DWORD cb);

	// MoveEnd: Moves the end of the buffer.
	// Call this method after reading data into the buffer.
	void MoveEnd(DWORD cb);

private:
	property BYTE *Ptr { BYTE *get() { return m_array->Data; } }

	void SetSize(DWORD count);
	void Allocate(DWORD alloc);

	property DWORD  CurrentFreeSize { DWORD get() const; }

private:

	Array<BYTE> ^m_array;
	DWORD m_count;        // Nominal count.
	DWORD m_allocated;    // Actual allocation size.

	DWORD m_begin;
	DWORD m_end;  // 1 past the last element
};

#include <queue>

// Parser class:
// Parses an MPEG-1 systems-layer stream.
ref class Parser sealed
{
internal:
	Parser();

	bool ParseBytes(const BYTE *pData, DWORD cbLen, DWORD *pAte);

	property bool HasFinishedParsedData{bool get() const { return m_isFinishedParsingMaster; }}
	MKVMasterData* GetMasterData();

	//property bool HasBlock {bool get() const {return }}
	QWORD	m_jumpTo;
	bool	m_jumpFlag;
	bool	m_isFinishedParsingMaster;
	//property bool HasSystemHeader{bool get() const { return m_header != nullptr; }}
	//ExpandableStruct<MPEG1SystemHeader> ^GetSystemHeader();

	bool				m_isCurrentKeyFrame;
	UINT64				m_currentBlockTimeCode;
	UINT64				m_currentTimeStamp;
	int					m_currentFrameSize;
	int					m_currentStream;
	bool				m_insertedHeaderYet;
	
	const static int	m_cirBufferLength = 30;
	int					m_circularBuffer[m_cirBufferLength];
	int*				pCircRead;
	int*				pCircWrite;
	int					m_circularBufferPosition[m_cirBufferLength];
	int*				pCircReadPosition;
	int*				pCircWritePosition;

	byte				m_frameCount;

	PROPVARIANT			m_startPosition;
	UINT64				FindSeekPoint();

	property std::queue<int> GetFrameSizeQueue {std::queue<int> get() const { return m_frameSizeQueue; }}
	void	PopFrameSizeQueue() { m_frameSizeQueue.pop(); }
	property bool HasFrames {bool get() const { return m_framesReady; }}
	//property const MPEG1PacketHeader &PacketHeader { const MPEG1PacketHeader &get() { assert(m_bHasPacketHeader); return m_curPacketHeader; } }

	
	//property DWORD PayloadSize{DWORD get() const { assert(m_bHasPacketHeader); return m_curPacketHeader.cbPayload; }}
	void ClearFrames() { m_framesReady = false; }

	property bool IsEndOfStream {bool get() const { return m_bEOS; }}

private:
	__int64* ParseFixedLengthNumber(byte* data, uint8 pos, uint8 length, bool _signed);
	__int64* ReadFixedLengthNumber(const BYTE **pData, DWORD *cbLen, DWORD *pAte, DWORD length, bool _signed);
	//void* ReadSimpleElement(const BYTE **pData, DWORD *cbLen, DWORD *pAte, EET type, DWORD size);

	base_element* ReadSimpleElement2(const BYTE **pData, DWORD *cbLen, DWORD *pAte, EET type, DWORD size);
	bit_number_result GetMajorBitNumber(uint8 num);
	matroska_number_result ReadMatroskaNumber(const BYTE **pData, DWORD *cbLen, DWORD *pAte, bool unmodified, bool _signed);
	matroska_number_result ParseMatroskaNumber(BYTE **pData, bool isSigned, bool unModified);
	element_header_result ReadEbmlElementHeader(const BYTE **pData, DWORD *cbLen, DWORD *pAte);
	//void* ReadEbmlElementTree(const BYTE **pData, DWORD *cbLen, DWORD *pAte, DWORD total_size);
	master_element* Parser::ReadEbmlElementTree2(const BYTE **pData, DWORD *cbLen, DWORD *pAte, DWORD total_size);

	bool FindNextStartCode(const BYTE *pData, DWORD cbLen, DWORD *pAte);
	/*bool ParsePackHeader(const BYTE *pData, DWORD cbLen, DWORD *pAte);
	bool ParseSystemHeader(const BYTE *pData, DWORD cbLen, DWORD *pAte);
	bool ParsePacketHeader(const BYTE *pData, DWORD cbLen, DWORD *pAte);*/
	void OnEndOfStream();

private:
	
	std::queue<int>		m_frameSizeQueue;
	
	LONGLONG m_SCR;
	DWORD m_muxRate;
	Array<BYTE> ^m_data;

	MKVMasterData* m_masterData;
	//ExpandableStruct<MKVMasterData> ^m_masterData;

	ExpandableStruct<MPEG1SystemHeader> ^m_header;
	// Note: Size of header = sizeof(MPEG1SystemHeader) + (sizeof(MPEG1StreamHeader) * (cStreams - 1))

	bool m_framesReady;
	MPEG1PacketHeader m_curPacketHeader;  // Most recent packet header.

	bool m_bEOS;

	

};


DWORD ReadVideoSequenceHeader(_In_reads_bytes_(cbData) const BYTE *pData, DWORD cbData, MPEG1VideoSeqHeader &seqHeader);

DWORD ReadAudioFrameHeader(const BYTE *pData, DWORD cbData, MPEG1AudioFrameHeader &audioHeader);
