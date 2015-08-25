//////////////////////////////////////////////////////////////////////////
//
// Parse.cpp
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

#include "pch.h"

#include <list>
#include <map>
#include <iostream>
#include <sstream>
#include <iomanip>
#include "MKVSource.h"
#include "Parse.h"



// HAS_FLAG: Test if 'b' contains a specified bit flag
#define HAS_FLAG(b, flag) (((b) & (flag)) == (flag))

#define htonl(x)   ((((x) >> 24) & 0x000000FFL) | \
    (((x) >>  8) & 0x0000FF00L) | \
    (((x) <<  8) & 0x00FF0000L) | \
    (((x) << 24) & 0xFF000000L))

// MAKE_WORD: Convert two bytes into a WORD
inline WORD MAKE_WORD(BYTE b1, BYTE b2)
{
	return ((b1 << 8) | b2);
}

// MAKE_DWORD_HOSTORDER:
// Convert the first 4 bytes of an array into a DWORD in host order (ie, no byte swapping).
inline DWORD MAKE_DWORD_HOSTORDER(const BYTE *pData)
{
	return ((DWORD*)pData)[0];
}

// MAKE_DWORD:
// Convert the first 4 bytes of an array into a DWORD in MPEG-1 stream byte order.
inline DWORD MAKE_DWORD(const BYTE *pData)
{
	return htonl(MAKE_DWORD_HOSTORDER(pData));
}


//-------------------------------------------------------------------
// AdvanceBufferPointer
// Advances a byte array pointer.
//
// pData: The array pointer.
// cbBufferSize: The array size. On output, the size remaining.
// cbAdvance: Number of bytes to advance the pointer.
//
// This function is just a helper for keeping a valid pointer as we
// walk through a buffer.
//-------------------------------------------------------------------

inline void AdvanceBufferPointer(const BYTE* &pData, DWORD &cbBufferSize, DWORD cbAdvance)
{
	assert(cbBufferSize >= cbAdvance);
	if (cbBufferSize < cbAdvance)
	{
		throw ref new InvalidArgumentException();
	}
	cbBufferSize -= cbAdvance;
	pData += cbAdvance;
}

// ValidateBufferSize:
// Fails if cbBufferSize < cbMinRequiredSize.
inline void ValidateBufferSize(DWORD cbBufferSize, DWORD cbMinRequiredSize)
{
	if (cbBufferSize < cbMinRequiredSize)
	{
		ThrowException(MF_E_INVALID_FORMAT);
	}
}

// Forward declarations.
void ParseStreamData(const BYTE *pData, MPEG1StreamHeader &header);
void ParseStreamId(BYTE id, StreamType *pType, BYTE *pStreamNum);
LONGLONG ParsePTS(const BYTE *pData);

MFRatio GetFrameRate(BYTE frameRateCode);
MFRatio GetPixelAspectRatio(BYTE pixelAspectCode);

DWORD GetAudioBitRate(MPEG1AudioLayer layer, BYTE index);
DWORD GetSamplingFrequency(BYTE code);


//-------------------------------------------------------------------
// Buffer class
//-------------------------------------------------------------------


Buffer::Buffer(DWORD cbSize)
	: m_begin(0)
	, m_end(0)
	, m_count(0)
	, m_allocated(0)
{
	SetSize(cbSize);
	ZeroMemory(Ptr, cbSize);
}

//-------------------------------------------------------------------
// DataPtr
// Returns a pointer to the start of the buffer.
//-------------------------------------------------------------------

BYTE *Buffer::DataPtr::get()
{
	return Ptr + m_begin;
}


//-------------------------------------------------------------------
// DataSize
// Returns the size of the buffer.
//
// Note: The "size" is determined by the start and end pointers.
// The memory allocated for the buffer can be larger.
//-------------------------------------------------------------------

DWORD Buffer::DataSize::get() const
{
	assert(m_end >= m_begin);

	return m_end - m_begin;
}

// Reserves memory for the array, but does not increase the count.
void Buffer::Allocate(DWORD alloc)
{
	HRESULT hr = S_OK;
	if (alloc > m_allocated)
	{
		Array<BYTE> ^tmp = ref new Array<BYTE>(alloc);
		ZeroMemory(tmp->Data, alloc);

		assert(m_count <= m_allocated);

		// Copy the elements to the re-allocated array.
		for (DWORD i = 0; i < m_count; i++)
		{
			tmp[i] = m_array[i];
		}
		m_array = tmp;
		m_allocated = alloc;
	}
}

// Changes the count, and grows the array if needed.
void Buffer::SetSize(DWORD count)
{
	assert(m_count <= m_allocated);

	if (count > m_allocated)
	{
		Allocate(count);
	}

	m_count = count;
}


//-------------------------------------------------------------------
// Reserve
// Reserves additional bytes of memory for the buffer.
//
// This method does *not *increase the value returned by DataSize().
//
// After this method returns, the value of DataPtr() might change,
// so do not cache the old value.
//-------------------------------------------------------------------

void Buffer::Reserve(DWORD cb)
{
	if (cb > MAXDWORD - DataSize)
	{
		throw ref new InvalidArgumentException();
	}

	// If this would push the end position past the end of the array,
	// then we need to copy up the data to start of the array. We might
	// also need to realloc the array.

	if (cb > m_count - m_end)
	{
		// New end position would be past the end of the array.
		// Check if we need to grow the array.

		if (cb > CurrentFreeSize)
		{
			// Array needs to grow
			SetSize(DataSize + cb);
		}

		MoveMemory(Ptr, DataPtr, DataSize);

		// Reset begin and end.
		m_end = DataSize; // Update m_end first before resetting m_begin!
		m_begin = 0;
	}

	assert(CurrentFreeSize >= cb);
}


//-------------------------------------------------------------------
// MoveStart
// Moves the start of the buffer by cb bytes.
//
// Call this method after consuming data from the buffer.
//-------------------------------------------------------------------

void Buffer::MoveStart(DWORD cb)
{
	// Cannot advance pass the end of the buffer.
	if (cb > DataSize)
	{
		throw ref new InvalidArgumentException();
	}

	m_begin += cb;
}


//-------------------------------------------------------------------
// MoveEnd
// Moves end position of the buffer.
//-------------------------------------------------------------------

void Buffer::MoveEnd(DWORD cb)
{
	Reserve(cb);
	m_end += cb;
}


//-------------------------------------------------------------------
// CurrentFreeSize (private)
//
// Returns the size of the array minus the size of the data.
//-------------------------------------------------------------------

DWORD Buffer::CurrentFreeSize::get() const
{
	assert(m_count >= DataSize);
	return m_count - DataSize;
}


//-------------------------------------------------------------------
// Parser class
//-------------------------------------------------------------------


Parser::Parser()
	: m_SCR(0)
	, m_muxRate(0)
	, m_framesReady(false)
	, m_bEOS(false)
	, m_isFinishedParsingMaster(false)
	, m_jumpFlag(false)
	, m_insertedHeaderYet(false)
	, pCircRead(&m_circularBuffer[0])
	, pCircWrite(&m_circularBuffer[0])
	, pCircReadPosition(&m_circularBufferPosition[0])
	, pCircWritePosition(&m_circularBufferPosition[0])
{
	ZeroMemory(&m_curPacketHeader, sizeof(m_curPacketHeader));

	m_masterData = new MKVMasterData();
}

//-------------------------------------------------------------------
// GetSystemHeader class
//
// Returns a copy of the system header.
// Do not call this method unless HasSystemHeader() returns true.
//
// The caller must free the returned structure by calling
// CoTaskMemFree.
//-------------------------------------------------------------------

//ExpandableStruct<MPEG1SystemHeader> ^Parser::GetSystemHeader()
//{
//	if (!HasSystemHeader)
//	{
//		ThrowException(MF_INVALID_STATE_ERR);
//	}
//
//	assert(m_header->Size > 0);
//
//	auto header = ref new ExpandableStruct<MPEG1SystemHeader>(m_header);
//
//	header->CopyFrom(m_header);
//
//	return header;
//}

MKVMasterData* Parser::GetMasterData()
{
	/*if (!HasFinishedParsedData)
	{
		ThrowException(MF_INVALID_STATE_ERR);
	}*/

	//assert(m_header->Size > 0);

	//auto header = ref new ExpandableStruct<MKVMasterData>(m_masterData);

	//header->CopyFrom(m_masterData);
	
	return m_masterData;
}


std::map<DWORD, type_name > element_types_names =
{
	std::make_pair(0x1A45DFA3, type_name(EET::MASTER, "EBML")),
	std::make_pair(0x4286, type_name(EET::_UNSIGNED, "EBMLVersion")),
	std::make_pair(0x42F7, type_name(EET::_UNSIGNED, "EBMLReadVersion")),
	std::make_pair(0x42F2, type_name(EET::_UNSIGNED, "EBMLMaxIDLength")),
	std::make_pair(0x42F3, type_name(EET::_UNSIGNED, "EBMLMaxSizeLength")),
	std::make_pair(0x4282, type_name(EET::TEXTA, "DocType")),
	std::make_pair(0x4287, type_name(EET::_UNSIGNED, "DocTypeVersion")),
	std::make_pair(0x4285, type_name(EET::_UNSIGNED, "DocTypeReadVersion")),
	std::make_pair(0xEC, type_name(EET::BINARY, "Void")),
	std::make_pair(0xBF, type_name(EET::BINARY, "CRC-32")),
	std::make_pair(0x1B538667, type_name(EET::MASTER, "SignatureSlot")),
	std::make_pair(0x7E8A, type_name(EET::_UNSIGNED, "SignatureAlgo")),
	std::make_pair(0x7E9A, type_name(EET::_UNSIGNED, "SignatureHash")),
	std::make_pair(0x7EA5, type_name(EET::BINARY, "SignaturePublicKey")),
	std::make_pair(0x7EB5, type_name(EET::BINARY, "Signature")),
	std::make_pair(0x7E5B, type_name(EET::MASTER, "SignatureElements")),
	std::make_pair(0x7E7B, type_name(EET::MASTER, "SignatureElementList")),
	std::make_pair(0x6532, type_name(EET::BINARY, "SignedElement")),
	std::make_pair(0x18538067, type_name(EET::JUST_GO_ON, "Segment")),
	std::make_pair(0x114D9B74, type_name(EET::MASTER, "SeekHead")),
	std::make_pair(0x4DBB, type_name(EET::MASTER, "Seek")),
	std::make_pair(0x53AB, type_name(EET::BINARY, "SeekID")),
	std::make_pair(0x53AC, type_name(EET::_UNSIGNED, "SeekPosition")),
	std::make_pair(0x1549A966, type_name(EET::MASTER, "Info")),
	std::make_pair(0x73A4, type_name(EET::BINARY, "SegmentUID")),
	std::make_pair(0x7384, type_name(EET::TEXTU, "SegmentFilename")),
	std::make_pair(0x3CB923, type_name(EET::BINARY, "PrevUID")),
	std::make_pair(0x3C83AB, type_name(EET::TEXTU, "PrevFilename")),
	std::make_pair(0x3EB923, type_name(EET::BINARY, "NextUID")),
	std::make_pair(0x3E83BB, type_name(EET::TEXTU, "NextFilename")),
	std::make_pair(0x4444, type_name(EET::BINARY, "SegmentFamily")),
	std::make_pair(0x6924, type_name(EET::MASTER, "ChapterTranslate")),
	std::make_pair(0x69FC, type_name(EET::_UNSIGNED, "ChapterTranslateEditionUID")),
	std::make_pair(0x69BF, type_name(EET::_UNSIGNED, "ChapterTranslateCodec")),
	std::make_pair(0x69A5, type_name(EET::BINARY, "ChapterTranslateID")),
	std::make_pair(0x2AD7B1, type_name(EET::_UNSIGNED, "TimecodeScale")),
	std::make_pair(0x4489, type_name(EET::_FLOAT, "Duration")),
	std::make_pair(0x4461, type_name(EET::_DATE, "DateUTC")),
	std::make_pair(0x7BA9, type_name(EET::TEXTU, "Title")),
	std::make_pair(0x4D80, type_name(EET::TEXTU, "MuxingApp")),
	std::make_pair(0x5741, type_name(EET::TEXTU, "WritingApp")),
	std::make_pair(0x1F43B675, type_name(EET::JUST_GO_ON, "Cluster")),
	std::make_pair(0xE7, type_name(EET::_UNSIGNED, "Timecode")),
	std::make_pair(0x5854, type_name(EET::MASTER, "SilentTracks")),
	std::make_pair(0x58D7, type_name(EET::_UNSIGNED, "SilentTrackNumber")),
	std::make_pair(0xA7, type_name(EET::_UNSIGNED, "Position")),
	std::make_pair(0xAB, type_name(EET::_UNSIGNED, "PrevSize")),
	std::make_pair(0xA3, type_name(EET::BINARY, "SimpleBlock")),
	std::make_pair(0xA0, type_name(EET::MASTER, "BlockGroup")),
	std::make_pair(0xA1, type_name(EET::BINARY, "Block")),
	std::make_pair(0xA2, type_name(EET::BINARY, "BlockVirtual")),
	std::make_pair(0x75A1, type_name(EET::MASTER, "BlockAdditions")),
	std::make_pair(0xA6, type_name(EET::MASTER, "BlockMore")),
	std::make_pair(0xEE, type_name(EET::_UNSIGNED, "BlockAddID")),
	std::make_pair(0xA5, type_name(EET::BINARY, "BlockAdditional")),
	std::make_pair(0x9B, type_name(EET::_UNSIGNED, "BlockDuration")),
	std::make_pair(0xFA, type_name(EET::_UNSIGNED, "ReferencePriority")),
	std::make_pair(0xFB, type_name(EET::_SIGNED, "ReferenceBlock")),
	std::make_pair(0xFD, type_name(EET::_SIGNED, "ReferenceVirtual")),
	std::make_pair(0xA4, type_name(EET::BINARY, "CodecState")),
	std::make_pair(0x8E, type_name(EET::MASTER, "Slices")),
	std::make_pair(0xE8, type_name(EET::MASTER, "TimeSlice")),
	std::make_pair(0xCC, type_name(EET::_UNSIGNED, "LaceNumber")),
	std::make_pair(0xCD, type_name(EET::_UNSIGNED, "FrameNumber")),
	std::make_pair(0xCB, type_name(EET::_UNSIGNED, "BlockAdditionID")),
	std::make_pair(0xCE, type_name(EET::_UNSIGNED, "Delay")),
	std::make_pair(0xCF, type_name(EET::_UNSIGNED, "SliceDuration")),
	std::make_pair(0xC8, type_name(EET::MASTER, "ReferenceFrame")),
	std::make_pair(0xC9, type_name(EET::_UNSIGNED, "ReferenceOffset")),
	std::make_pair(0xCA, type_name(EET::_UNSIGNED, "ReferenceTimeCode")),
	std::make_pair(0xAF, type_name(EET::BINARY, "EncryptedBlock")),
	std::make_pair(0x1654AE6B, type_name(EET::MASTER, "Tracks")),
	std::make_pair(0xAE, type_name(EET::MASTER, "TrackEntry")),
	std::make_pair(0xD7, type_name(EET::_UNSIGNED, "TrackNumber")),
	std::make_pair(0x73C5, type_name(EET::_UNSIGNED, "TrackUID")),
	std::make_pair(0x83, type_name(EET::_UNSIGNED, "TrackType")),
	std::make_pair(0xB9, type_name(EET::_UNSIGNED, "FlagEnabled")),
	std::make_pair(0x88, type_name(EET::_UNSIGNED, "FlagDefault")),
	std::make_pair(0x55AA, type_name(EET::_UNSIGNED, "FlagForced")),
	std::make_pair(0x9C, type_name(EET::_UNSIGNED, "FlagLacing")),
	std::make_pair(0x6DE7, type_name(EET::_UNSIGNED, "MinCache")),
	std::make_pair(0x6DF8, type_name(EET::_UNSIGNED, "MaxCache")),
	std::make_pair(0x23E383, type_name(EET::_UNSIGNED, "DefaultDuration")),
	std::make_pair(0x23314F, type_name(EET::_FLOAT, "TrackTimecodeScale")),
	std::make_pair(0x537F, type_name(EET::_SIGNED, "TrackOffset")),
	std::make_pair(0x55EE, type_name(EET::_UNSIGNED, "MaxBlockAdditionID")),
	std::make_pair(0x536E, type_name(EET::TEXTU, "Name")),
	std::make_pair(0x22B59C, type_name(EET::TEXTA, "Language")),
	std::make_pair(0x86, type_name(EET::TEXTA, "CodecID")),
	std::make_pair(0x63A2, type_name(EET::BINARY, "CodecPrivate")),
	std::make_pair(0x258688, type_name(EET::TEXTU, "CodecName")),
	std::make_pair(0x7446, type_name(EET::_UNSIGNED, "AttachmentLink")),
	std::make_pair(0x3A9697, type_name(EET::TEXTU, "CodecSettings")),
	std::make_pair(0x3B4040, type_name(EET::TEXTA, "CodecInfoURL")),
	std::make_pair(0x26B240, type_name(EET::TEXTA, "CodecDownloadURL")),
	std::make_pair(0xAA, type_name(EET::_UNSIGNED, "CodecDecodeAll")),
	std::make_pair(0x6FAB, type_name(EET::_UNSIGNED, "TrackOverlay")),
	std::make_pair(0x6624, type_name(EET::MASTER, "TrackTranslate")),
	std::make_pair(0x66FC, type_name(EET::_UNSIGNED, "TrackTranslateEditionUID")),
	std::make_pair(0x66BF, type_name(EET::_UNSIGNED, "TrackTranslateCodec")),
	std::make_pair(0x66A5, type_name(EET::BINARY, "TrackTranslateTrackID")),
	std::make_pair(0xE0, type_name(EET::MASTER, "Video")),
	std::make_pair(0x9A, type_name(EET::_UNSIGNED, "FlagInterlaced")),
	std::make_pair(0x53B8, type_name(EET::_UNSIGNED, "StereoMode")),
	std::make_pair(0x53B9, type_name(EET::_UNSIGNED, "OldStereoMode")),
	std::make_pair(0xB0, type_name(EET::_UNSIGNED, "PixelWidth")),
	std::make_pair(0xBA, type_name(EET::_UNSIGNED, "PixelHeight")),
	std::make_pair(0x54AA, type_name(EET::_UNSIGNED, "PixelCropBottom")),
	std::make_pair(0x54BB, type_name(EET::_UNSIGNED, "PixelCropTop")),
	std::make_pair(0x54CC, type_name(EET::_UNSIGNED, "PixelCropLeft")),
	std::make_pair(0x54DD, type_name(EET::_UNSIGNED, "PixelCropRight")),
	std::make_pair(0x54B0, type_name(EET::_UNSIGNED, "DisplayWidth")),
	std::make_pair(0x54BA, type_name(EET::_UNSIGNED, "DisplayHeight")),
	std::make_pair(0x54B2, type_name(EET::_UNSIGNED, "DisplayUnit")),
	std::make_pair(0x54B3, type_name(EET::_UNSIGNED, "AspectRatioType")),
	std::make_pair(0x2EB524, type_name(EET::BINARY, "ColourSpace")),
	std::make_pair(0x2FB523, type_name(EET::_FLOAT, "GammaValue")),
	std::make_pair(0x2383E3, type_name(EET::_FLOAT, "FrameRate")),
	std::make_pair(0xE1, type_name(EET::MASTER, "Audio")),
	std::make_pair(0xB5, type_name(EET::_FLOAT, "SamplingFrequency")),
	std::make_pair(0x78B5, type_name(EET::_FLOAT, "OutputSamplingFrequency")),
	std::make_pair(0x9F, type_name(EET::_UNSIGNED, "Channels")),
	std::make_pair(0x7D7B, type_name(EET::BINARY, "ChannelPositions")),
	std::make_pair(0x6264, type_name(EET::_UNSIGNED, "BitDepth")),
	std::make_pair(0xE2, type_name(EET::MASTER, "TrackOperation")),
	std::make_pair(0xE3, type_name(EET::MASTER, "TrackCombinePlanes")),
	std::make_pair(0xE4, type_name(EET::MASTER, "TrackPlane")),
	std::make_pair(0xE5, type_name(EET::_UNSIGNED, "TrackPlaneUID")),
	std::make_pair(0xE6, type_name(EET::_UNSIGNED, "TrackPlaneType")),
	std::make_pair(0xE9, type_name(EET::MASTER, "TrackJoinBlocks")),
	std::make_pair(0xED, type_name(EET::_UNSIGNED, "TrackJoinUID")),
	std::make_pair(0xC0, type_name(EET::_UNSIGNED, "TrickTrackUID")),
	std::make_pair(0xC1, type_name(EET::BINARY, "TrickTrackSegmentUID")),
	std::make_pair(0xC6, type_name(EET::_UNSIGNED, "TrickTrackFlag")),
	std::make_pair(0xC7, type_name(EET::_UNSIGNED, "TrickMasterTrackUID")),
	std::make_pair(0xC4, type_name(EET::BINARY, "TrickMasterTrackSegmentUID")),
	std::make_pair(0x6D80, type_name(EET::MASTER, "ContentEncodings")),
	std::make_pair(0x6240, type_name(EET::MASTER, "ContentEncoding")),
	std::make_pair(0x5031, type_name(EET::_UNSIGNED, "ContentEncodingOrder")),
	std::make_pair(0x5032, type_name(EET::_UNSIGNED, "ContentEncodingScope")),
	std::make_pair(0x5033, type_name(EET::_UNSIGNED, "ContentEncodingType")),
	std::make_pair(0x5034, type_name(EET::MASTER, "ContentCompression")),
	std::make_pair(0x4254, type_name(EET::_UNSIGNED, "ContentCompAlgo")),
	std::make_pair(0x4255, type_name(EET::BINARY, "ContentCompSettings")),
	std::make_pair(0x5035, type_name(EET::MASTER, "ContentEncryption")),
	std::make_pair(0x47E1, type_name(EET::_UNSIGNED, "ContentEncAlgo")),
	std::make_pair(0x47E2, type_name(EET::BINARY, "ContentEncKeyID")),
	std::make_pair(0x47E3, type_name(EET::BINARY, "ContentSignature")),
	std::make_pair(0x47E4, type_name(EET::BINARY, "ContentSigKeyID")),
	std::make_pair(0x47E5, type_name(EET::_UNSIGNED, "ContentSigAlgo")),
	std::make_pair(0x47E6, type_name(EET::_UNSIGNED, "ContentSigHashAlgo")),
	std::make_pair(0x1C53BB6B, type_name(EET::MASTER, "Cues")),
	std::make_pair(0xBB, type_name(EET::MASTER, "CuePoint")),
	std::make_pair(0xB3, type_name(EET::_UNSIGNED, "CueTime")),
	std::make_pair(0xB7, type_name(EET::MASTER, "CueTrackPositions")),
	std::make_pair(0xF7, type_name(EET::_UNSIGNED, "CueTrack")),
	std::make_pair(0xF1, type_name(EET::_UNSIGNED, "CueClusterPosition")),
	std::make_pair(0x5378, type_name(EET::_UNSIGNED, "CueBlockNumber")),
	std::make_pair(0xEA, type_name(EET::_UNSIGNED, "CueCodecState")),
	std::make_pair(0xDB, type_name(EET::MASTER, "CueReference")),
	std::make_pair(0x96, type_name(EET::_UNSIGNED, "CueRefTime")),
	std::make_pair(0x97, type_name(EET::_UNSIGNED, "CueRefCluster")),
	std::make_pair(0x535F, type_name(EET::_UNSIGNED, "CueRefNumber")),
	std::make_pair(0xEB, type_name(EET::_UNSIGNED, "CueRefCodecState")),
	std::make_pair(0x1941A469, type_name(EET::MASTER, "Attachments")),
	std::make_pair(0x61A7, type_name(EET::MASTER, "AttachedFile")),
	std::make_pair(0x467E, type_name(EET::TEXTU, "FileDescription")),
	std::make_pair(0x466E, type_name(EET::TEXTU, "FileName")),
	std::make_pair(0x4660, type_name(EET::TEXTA, "FileMimeType")),
	std::make_pair(0x465C, type_name(EET::BINARY, "FileData")),
	std::make_pair(0x46AE, type_name(EET::_UNSIGNED, "FileUID")),
	std::make_pair(0x4675, type_name(EET::BINARY, "FileReferral")),
	std::make_pair(0x4661, type_name(EET::_UNSIGNED, "FileUsedStartTime")),
	std::make_pair(0x4662, type_name(EET::_UNSIGNED, "FileUsedEndTime")),
	std::make_pair(0x1043A770, type_name(EET::MASTER, "Chapters")),
	std::make_pair(0x45B9, type_name(EET::MASTER, "EditionEntry")),
	std::make_pair(0x45BC, type_name(EET::_UNSIGNED, "EditionUID")),
	std::make_pair(0x45BD, type_name(EET::_UNSIGNED, "EditionFlagHidden")),
	std::make_pair(0x45DB, type_name(EET::_UNSIGNED, "EditionFlagDefault")),
	std::make_pair(0x45DD, type_name(EET::_UNSIGNED, "EditionFlagOrdered")),
	std::make_pair(0xB6, type_name(EET::MASTER, "ChapterAtom")),
	std::make_pair(0x73C4, type_name(EET::_UNSIGNED, "ChapterUID")),
	std::make_pair(0x91, type_name(EET::_UNSIGNED, "ChapterTimeStart")),
	std::make_pair(0x92, type_name(EET::_UNSIGNED, "ChapterTimeEnd")),
	std::make_pair(0x98, type_name(EET::_UNSIGNED, "ChapterFlagHidden")),
	std::make_pair(0x4598, type_name(EET::_UNSIGNED, "ChapterFlagEnabled")),
	std::make_pair(0x6E67, type_name(EET::BINARY, "ChapterSegmentUID")),
	std::make_pair(0x6EBC, type_name(EET::_UNSIGNED, "ChapterSegmentEditionUID")),
	std::make_pair(0x63C3, type_name(EET::_UNSIGNED, "ChapterPhysicalEquiv")),
	std::make_pair(0x8F, type_name(EET::MASTER, "ChapterTrack")),
	std::make_pair(0x89, type_name(EET::_UNSIGNED, "ChapterTrackNumber")),
	std::make_pair(0x80, type_name(EET::MASTER, "ChapterDisplay")),
	std::make_pair(0x85, type_name(EET::TEXTU, "ChapString")),
	std::make_pair(0x437C, type_name(EET::TEXTA, "ChapLanguage")),
	std::make_pair(0x437E, type_name(EET::TEXTA, "ChapCountry")),
	std::make_pair(0x6944, type_name(EET::MASTER, "ChapProcess")),
	std::make_pair(0x6955, type_name(EET::_UNSIGNED, "ChapProcessCodecID")),
	std::make_pair(0x450D, type_name(EET::BINARY, "ChapProcessPrivate")),
	std::make_pair(0x6911, type_name(EET::MASTER, "ChapProcessCommand")),
	std::make_pair(0x6922, type_name(EET::_UNSIGNED, "ChapProcessTime")),
	std::make_pair(0x6933, type_name(EET::BINARY, "ChapProcessData")),
	std::make_pair(0x1254C367, type_name(EET::MASTER, "Tags")),
	std::make_pair(0x7373, type_name(EET::MASTER, "Tag")),
	std::make_pair(0x63C0, type_name(EET::MASTER, "Targets")),
	std::make_pair(0x68CA, type_name(EET::_UNSIGNED, "TargetTypeValue")),
	std::make_pair(0x63CA, type_name(EET::TEXTA, "TargetType")),
	std::make_pair(0x63C5, type_name(EET::_UNSIGNED, "TagTrackUID")),
	std::make_pair(0x63C9, type_name(EET::_UNSIGNED, "TagEditionUID")),
	std::make_pair(0x63C4, type_name(EET::_UNSIGNED, "TagChapterUID")),
	std::make_pair(0x63C6, type_name(EET::_UNSIGNED, "TagAttachmentUID")),
	std::make_pair(0x67C8, type_name(EET::MASTER, "SimpleTag")),
	std::make_pair(0x45A3, type_name(EET::TEXTU, "TagName")),
	std::make_pair(0x447A, type_name(EET::TEXTA, "TagLanguage")),
	std::make_pair(0x4484, type_name(EET::_UNSIGNED, "TagDefault")),
	std::make_pair(0x4487, type_name(EET::TEXTU, "TagString")),
	std::make_pair(0x4485, type_name(EET::BINARY, "TagBinary"))

};

__int64* Parser::ParseFixedLengthNumber(byte* data, uint8 pos, uint8 length, bool _signed)
{
	__int64 *r = new __int64(0);
	for (int i = 0; i < length; i++)
	{
		*r = *r * 0x100 + data[pos + i];
	}
	if (_signed)
	{
		if (data[pos] & 0x80)
		{
			*r -= pow(2, 8 * length);
		}
	}
	//__int64 s = r[0];
	//delete [] r;
	//num_pos *np = new num_pos(r, pos + length);
	return r;
}

__int64* Parser::ReadFixedLengthNumber(const BYTE **pData, DWORD *cbLen, DWORD *pAte, DWORD length, bool _signed = false)
{
	//Read length bytes and parse (parse_fixedlength_number) it. 
	//Returns only the number
	byte buf[8];
	for (int i = 0; i < length; i++)
	{
		buf[i] = **pData;
		(*pData)++;
		(*cbLen)--;
		(*pAte)++;
	}
	//num_pos *npResult = ParseFixedLengthNumber(buf, 0, length, _signed);
	//return (*npResult).num;
	return ParseFixedLengthNumber(buf, 0, length, _signed);
}



bit_number_result Parser::GetMajorBitNumber(uint8 n)
{
	auto i = 0x80;
	auto r = 0;
	while (!(n&i))
	{
		r += 1;
		i >>= 1;
	}
	//bit_number_result result;
	//result.bitNum = r;
	//result.clearedNum = (n &~i);
	return bit_number_result(r, (n &~i));
}

matroska_number_result Parser::ReadMatroskaNumber(const BYTE **pData, DWORD *cbLen, DWORD *pAte, bool unmodified = false, bool _signed = false)
{
	if (unmodified && _signed)
	{
		throw ref new Exception(-1, L"Contradictory arguments: ReadMatroskaNumber unmodified and signed");
	}
	matroska_number_result mresult;
	DWORD code = **pData;
	(*pData)++;
	(*cbLen)--;
	(*pAte)++;
	bit_number_result numResult = GetMajorBitNumber(code);
	if (!unmodified)
		code = numResult.clearedNum;
	//signed means negative now
	auto i = numResult.bitNum;
	while (i)
	{
		code = code * 0x100 + **pData;
		(*pData)++;
		(*cbLen)--;
		(*pAte)++;
		i -= 1;
	}
	if (_signed)
		code -= (pow(2,(7 * numResult.bitNum + 7)) - 1);
	else
	{
		if (code == (pow(2, 7 * numResult.bitNum + 7) - 1))
		{
			mresult.id = -1;
			mresult.length = numResult.bitNum + 1;
			return mresult;
		}
	}
	mresult.id = code;
	mresult.length = numResult.bitNum + 1;
	return mresult;
}

element_header_result Parser::ReadEbmlElementHeader(const BYTE **pData, DWORD *cbLen, DWORD *pAte)
{
	matroska_number_result mresult1 = ReadMatroskaNumber(pData, cbLen, pAte, true);
	matroska_number_result mresult2 = ReadMatroskaNumber(pData, cbLen, pAte);
	element_header_result result;
	result.id = mresult1.id;
	result.elemsize = mresult2.id;
	result.headsize = mresult1.length + mresult2.length;
	return result;
}

master_element* Parser::ReadEbmlElementTree2(const BYTE **pData, DWORD *cbLen, DWORD *pAte, DWORD total_size)
{
	master_element* melement = new master_element();
	
	while (total_size > 0)
	{
		element_header_result hresult = ReadEbmlElementHeader(pData, cbLen, pAte);
		if (hresult.elemsize == -1)
		{
			//skipping data, error
			*pData += total_size;
			*cbLen -= total_size;
			*pAte = *pAte + total_size;
			break;
		}
		if (hresult.elemsize > total_size)
		{
			//skipping data, error
			*pData += total_size;
			*cbLen -= total_size;
			*pAte = *pAte + total_size;
		}
		auto type = EET::BINARY;
		int len = 9;
		//		char buffer[8];
		//		itoa(hresult.id, buffer, 16);
		std::stringstream ss;
		ss << std::setfill('0');
		ss << std::hex << hresult.id << std::dec << std::endl;
		const char* name = nullptr;
		//char* name = new char[30];
		//strcpy_s(name, 30, ("unknown_" + ss.str()).c_str());

		type_name typeNameResult;
		if (element_types_names.count(hresult.id) > 0)
		{
			typeNameResult = element_types_names.find(hresult.id)->second;
			name = typeNameResult.name;
			//strcpy_s(name, 30, typeNameResult.name);
			type = typeNameResult.type;
		}
		if (type == EET::MASTER)
		{
			auto masterElement = ReadEbmlElementTree2(pData, cbLen, pAte, hresult.elemsize);
			//strcpy_s(masterElement->name, typeNameResult.name);
			masterElement->name = typeNameResult.name;
			masterElement->type = typeNameResult.type;
			melement->children.push_back(masterElement);
		}
		else
		{
			auto simpleElement = ReadSimpleElement2(pData, cbLen, pAte, type, hresult.elemsize);
			//strcpy_s(simpleElement->name, typeNameResult.name);
			simpleElement->name = typeNameResult.name;
			melement->children.push_back(simpleElement); //std::pair<const char*,type_data>(name,type_data(type, d)));
		}
		total_size -= (hresult.elemsize + hresult.headsize);
		
	}
	return melement;
}

//void* Parser::ReadEbmlElementTree(const BYTE **pData, DWORD *cbLen, DWORD *pAte, DWORD total_size)
//{
//	//std::map<const char*, type_data> childs;
//
//	std::vector<child_element> childs;
//	//child_element child[];
//	
//	while (total_size > 0)
//	{
//		element_header_result hresult = ReadEbmlElementHeader(pData, cbLen, pAte);
//		if (hresult.elemsize == -1)
//		{
//			//skipping data, error
//			*pData += total_size;
//			*cbLen -= total_size;
//			*pAte = *pAte + total_size;
//			break;
//		}
//		if (hresult.elemsize > total_size)
//		{
//			//skipping data, error
//			*pData += total_size;
//			*cbLen -= total_size;
//			*pAte = *pAte + total_size;
//		}
//		auto type = EET::BINARY;
//		int len = 9;
////		char buffer[8];
////		itoa(hresult.id, buffer, 16);
//		std::stringstream ss;
//		ss << std::setfill('0');
//		ss << std::hex << hresult.id << std::dec << std::endl;
//		//char* name = new char[30];
//		const char* name = nullptr;
//		//strcpy_s(name,30, ("unknown_" + ss.str()).c_str());
//
//		type_name typeNameResult;
//		if (element_types_names.count(hresult.id) > 0)
//		{
//			typeNameResult = element_types_names.find(hresult.id)->second;
//			name = typeNameResult.name;
//			//strcpy_s(name,30,typeNameResult.name);
//			type = typeNameResult.type;
//		}
//		auto d = ReadSimpleElement(pData, cbLen, pAte, type, hresult.elemsize);
//		total_size -= (hresult.elemsize + hresult.headsize);
//		childs.push_back(child_element(name, type_data(type, d, hresult.elemsize))); //std::pair<const char*,type_data>(name,type_data(type, d)));
//	}
//	//convert map data to simple array since this can be assigned to a void pointer too... need standard lengths for strings.
//	child_element* childArray = new child_element[childs.size()];
//	auto i = 0;
//	//for (std::map<const char*, type_data>::iterator it = childs.begin(); it != childs.end(); ++it)
//	
//	for (std::vector<child_element>::iterator it = childs.begin(); it != childs.end(); ++it)
//	{
//		childArray[i] = child_element(it->name, it->typedata);
//		i++;
//	}
//
//	return &children(i, childArray);
//}


float Float(char unsigned *const p)
{
	float val;
	memcpy(&val, p, sizeof val);
	return val;
}

double Double(char unsigned *const p)
{
	double val;
	memcpy(&val, p, sizeof val);
	return val;
}

//template <class T>
base_element* Parser::ReadSimpleElement2(const BYTE **pData, DWORD *cbLen, DWORD *pAte, EET type, DWORD size)
{
	base_element* data;
	//void *data;
	if (size == 0)
		return 0;

	if (type == EET::_UNSIGNED)
	{
		auto d = (ReadFixedLengthNumber(pData, cbLen, pAte, size, false));
		auto uint = new uint_element();
		uint->data = *d;
		uint->type = type;
		data = uint;
		
	}
	else if (type == EET::_SIGNED)
	{
		auto d = ReadFixedLengthNumber(pData, cbLen, pAte, size, true);
		auto sint = new sint_element();
		sint->data = *d;
		sint->type = type;
		data = sint;
	}
	else if (type == EET::TEXTA)
	{
		char* buf = new char[size + 1];
		for (int i = 0; i < size; i++)
		{
			buf[i] = **pData;
			(*pData)++;
			(*cbLen)--;
			(*pAte)++;
		}
		buf[size] = '\0';
		auto el = new string_element();
		el->type = type;
		el->data = buf;
		data = el;
	}
	else if (type == EET::TEXTU)
	{
		char* buf = new char[size + 1];
		for (int i = 0; i < size; i++)
		{
			buf[i] = **pData;
			(*pData)++;
			(*cbLen)--;
			(*pAte)++;
		}
		buf[size] = '\0';
		auto el = new string_element();
		el->type = type;
		el->data = buf;
		data = el;
	}
	else if (type == EET::MASTER)
	{
		data = ReadEbmlElementTree2(pData, cbLen, pAte, size);
	}
	else if (type == EET::_DATE)
	{
		auto d = ReadFixedLengthNumber(pData, cbLen, pAte, size, true);
		*d /= 1000000000.0;
		*d += 978300000; // 2001 - 01 - 01T00:00 : 00, 000000000
		//data = d;  // this is now a UNIX date... may have to change 
		data = new base_element();
		data->type = type;
	}
	else if (type == EET::_FLOAT)
	{
		if (size == 4)
		{
			//char* buf = new char[size];
			char unsigned buf[4];
			for (int i = size - 1; i >= 0; i--)
			{
				buf[i] = **pData;
				(*pData)++;
				(*cbLen)--;
				(*pAte)++;
			}
			auto fl = Float(buf);
			//data = buf;
			auto el = new float_element();
			el->type = type;
			el->data = fl;
			data = el;
		}
		else if (size == 8)
		{
			char unsigned buf[8];
			for (int i = size - 1; i >= 0; i--)
			{
				buf[i] = **pData;
				(*pData)++;
				(*cbLen)--;
				(*pAte)++;
			}
			auto fl = Double(buf);
			//data = buf;
			auto el = new float_element();
			el->type = type;
			el->data = fl;
			data = el;
		}
		else
		{
			auto d = ReadFixedLengthNumber(pData, cbLen, pAte, size, false);
			delete d;
			OutputDebugString(L"cannot have float with non-standard length (not 4 or 8)");
			data = nullptr;
		}
	}
	else
	{
		byte* buf = new byte[size];
		for (int i = 0; i < size; i++)
		{
			buf[i] = **pData;
			(*pData)++;
			(*cbLen)--;
			(*pAte)++;
		}
		//data = buf;
		auto el = new binary_element();
		el->type = type;
		el->data = buf;
		el->length = size;
		data = el;
	}

	return data;
}


//void* Parser::ReadSimpleElement(const BYTE **pData, DWORD *cbLen, DWORD *pAte, EET type, DWORD size)
//{
//	void *data;
//	if (size == 0)
//		return 0;
//
//	if (type == EET::_UNSIGNED)
//	{
//		auto d = (ReadFixedLengthNumber(pData, cbLen, pAte, size, false));
//		data = d;
//	}
//	else if (type == EET::_SIGNED)
//	{
//		auto d = ReadFixedLengthNumber(pData, cbLen, pAte, size, true);
//		data = d;
//	}
//	else if (type == EET::TEXTA)
//	{
//		char* buf = new char[size+1];
//		for (int i = 0; i < size; i++)
//		{
//			buf[i] = **pData;
//			(*pData)++;
//			(*cbLen)--;
//			(*pAte)++;
//		}
//		buf[size] = '\0';
//		auto el = new string_element();
//		el->type = type;
//		el->data = buf;
//		data = el;
//	}
//	else if (type == EET::TEXTU)
//	{
//		char* buf = new char[size + 1];
//		for (int i = 0; i < size; i++)
//		{
//			buf[i] = **pData;
//			(*pData)++;
//			(*cbLen)--;
//			(*pAte)++;
//		}
//		buf[size] = '\0';
//		auto el = new string_element();
//		el->type = type;
//		el->data = buf;
//		data = el;
//	}
//	else if (type == EET::MASTER)
//	{
//		data = ReadEbmlElementTree(pData, cbLen, pAte, size);
//	}
//	else if (type == EET::_DATE)
//	{
//		auto d = ReadFixedLengthNumber(pData, cbLen, pAte, size, true);
//		*d /= 1000000000.0;
//		*d += 978300000; // 2001 - 01 - 01T00:00 : 00, 000000000
//
//		data = d;  // this is now a UNIX date... may have to change 
//	}
//	else if (type == EET::_FLOAT)
//	{
//		if (size == 4)
//		{
//			char* buf = new char[size];
//			for (int i = 0; i < size; i++)
//			{
//				buf[i] = **pData;
//				(*pData)++;
//				(*cbLen)--;
//				(*pAte)++;
//			}
//			data = buf;
//		}
//		else if (size == 8)
//		{
//			char* buf = new char[size];
//			for (int i = 0; i < size; i++)
//			{
//				buf[i] = **pData;
//				(*pData)++;
//				(*cbLen)--;
//				(*pAte)++;
//			}
//			data = buf;
//		}
//		else
//		{
//			auto d = ReadFixedLengthNumber(pData, cbLen, pAte, size, false);
//			OutputDebugString(L"cannot have float with non-standard length (not 4 or 8)");
//			data = nullptr;
//		}
//	}
//	else
//	{
//		byte* buf = new byte[size];
//		for (int i = 0; i < size; i++)
//		{
//			buf[i] = **pData;
//			(*pData)++;
//			(*cbLen)--;
//			(*pAte)++;
//		}
//		auto el = new binary_element();
//		el->type = type;
//		el->length = size;
//		el->data = buf;
//		data = el;
//	}
//
//	return data;
//}

matroska_number_result Parser::ParseMatroskaNumber(BYTE **pData, bool isSigned = false, bool unModified = false)
{
	matroska_number_result mresult;
	DWORD code = **pData;
	(*pData)++;
	bit_number_result numResult = GetMajorBitNumber(code);
	if (!unModified)
		code = numResult.clearedNum;
	//signed means negative now
	auto i = numResult.bitNum;
	while (i)
	{
		code = code * 0x100 + **pData;
		(*pData)++;
		i -= 1;
	}
	if (isSigned)
		code -= (pow(2, (7 * numResult.bitNum + 7)) - 1);
	else
	{
		if (code == (pow(2, 7 * numResult.bitNum + 7) - 1))
		{
			mresult.id = -1;
			mresult.length = numResult.bitNum + 1;
			return mresult;
		}
	}
	mresult.id = code;
	mresult.length = numResult.bitNum + 1;
	return mresult;
}

UINT64 Parser::FindSeekPoint()
{
	CuePoint* lastOne;
	auto scale = m_masterData->SegInfo->TimecodeScale/100 ;
	auto startTime = m_startPosition.hVal.QuadPart / scale;
	auto p = std::find_if(m_masterData->Cues.begin(), m_masterData->Cues.end(), [&lastOne, &startTime](CuePoint* item) {
		if (item->CueTime < startTime)
		{
			lastOne = item;
			return false;
		}
		else
			return true;
	});
	
	//for now, return the first track's position only
	
	//clear the seekpoint
	
	return lastOne->CueTrackPositions[0]->CueClusterPosition + m_masterData->SegmentPosition;
}

//-------------------------------------------------------------------
// ParseBytes
// Parses as much data as possible from the pData buffer, and returns
// the amount of data parsed in pAte (*pAte <= cbLen).
//
// Return values:
//      true: The method consumed some data (*pAte > 0).
//      false: The method did not consume any data (*pAte == 0).
//
// If the method returns S_FALSE, the caller must allocate a larger
// buffer and pass in more data.
//-------------------------------------------------------------------

bool Parser::ParseBytes(const BYTE *pData, DWORD cbLen, DWORD *pAte)
{
	bool result = true;

	*pAte = 0;

	EET type;
	int size;
	int hsize;
	void* data;
	std::string name = "";
	//std::vector<child_element> childs;
	master_element* masterElement = nullptr;
	//std::map<const char*, type_data, cmp_str> childs;

	if (cbLen < 4)
	{
		return false;
	}


	while (cbLen > 0)
	{
		try
		{
			element_header_result elemHeader = ReadEbmlElementHeader(&pData, &cbLen, pAte);
			type_name typeNameResult = element_types_names.find(elemHeader.id)->second;
			name = typeNameResult.name;
			type = typeNameResult.type;
			size = elemHeader.elemsize;
			hsize = elemHeader.headsize;
			

			if (typeNameResult.type == EET::MASTER)
			{
				if (size > cbLen)
				{
					pData -= hsize;
					cbLen += hsize;
					*pAte -= hsize;
					return false;
				}
				
				masterElement = ReadEbmlElementTree2(&pData, &cbLen, pAte, elemHeader.elemsize);
				//children result = *reinterpret_cast<children*>(ReadEbmlElementTree(&pData, &cbLen, pAte, elemHeader.elemsize));
				//for (int i = 0; i < result.count; i++)
				//{
				//	childs.push_back(child_element(result.elements[i].name, result.elements[i].typedata));  // std::pair<const char*, type_data>(result.elements[i].name, result.elements[i].typedata));
				//}
			}
			else if (typeNameResult.type == EET::JUST_GO_ON)
			{
				if (name == "Cluster")
				{
					if (!m_isFinishedParsingMaster)
					{
						for (int i = 0; i < m_masterData->SeekHead.size(); ++i)
						{
							if ((strcmp(m_masterData->SeekHead[i]->elemID, "Info") == 0 && m_masterData->SegInfo == NULL) 
								|| (strcmp(m_masterData->SeekHead[i]->elemID, "Tracks") == 0 && m_masterData->Tracks.size() == 0)
								//|| (strcmp(m_masterData->SeekHead[i]->elemID, "Tags") == 0 && m_masterData->Tags == NULL)
								|| (strcmp(m_masterData->SeekHead[i]->elemID, "Cues") == 0 && m_masterData->Cues.size() == 0))  //may fail if no cues are defined.
							{
								m_jumpTo = m_masterData->SeekHead[i]->SeekPosition + m_masterData->SegmentPosition;
								m_jumpFlag = true;
								return false;
							}
						}
						m_isFinishedParsingMaster = true;
						
						/*m_jumpTo = m_masterData->SeekHead[2]->SeekPosition + m_masterData->SegmentPosition;
						m_jumpFlag = true;*/
						
					}
				}
			}
			else
			{
				if (size > cbLen)
				{
					pData -= hsize;
					cbLen += hsize;
					*pAte -= hsize;
					return false;
				}
			}
		}
		catch (std::exception e)
		{
			auto i = 0;
		}

		if (name == "EBML")
		{
			//std::vector<child_element>::iterator it = std::find_if(childs.begin(), childs.end(), [](const child_element& e) -> bool {return strcmp(e.name ,"EBMLReadVersion") == 0; });
			//if (it != childs.end())
			//{
			//	auto val = reinterpret_cast<int*>(it->typedata.data);
			//	if (*reinterpret_cast<__int64*>(it->typedata.data) > 1)
			//		throw std::exception("EBMLReadVersion too big\n");
			//}
			//it = std::find_if(childs.begin(), childs.end(), [](const child_element& e) -> bool {return strcmp(e.name, "DocTypeReadVersion") == 0; });
			//if (it != childs.end())
			//{
			//	auto val = reinterpret_cast<int*>(it->typedata.data);
			//	if (*reinterpret_cast<__int64*>(it->typedata.data) > 2)
			//		throw std::exception("DocTypeReadVersion too big\n");
			//}
			//it = std::find_if(childs.begin(), childs.end(), [](const child_element& e) -> bool {return strcmp(e.name, "DocType") == 0; });
			//if (it != childs.end())
			//{
			//	auto dt = reinterpret_cast<const char*>(it->typedata.data);
			//	if ((strcmp(dt, "matroska") != 0) && (strcmp(dt, "webm") != 0))
			//		throw ref new Exception(-2, "EBML DocType is not matroska or webm");
			//}
			
		}
		else if (name == "Segment")
		{
			m_masterData->SegmentPosition = *pAte;
		}
		else if (name == "SeekHead")
		{
			if (masterElement != nullptr)
			{
				for (int i = 0; i < masterElement->children.size(); ++i)
				{
					auto base = masterElement->children[i];
					auto element = dynamic_cast<master_element*>(base);
					auto seek = new Seek();
					for (int j = 0; j < element->children.size(); ++j)
					{
						if (element->children[j]->type == EET::BINARY)
						{
							auto selement = dynamic_cast<binary_element*>(element->children[j]);
							auto arr = selement->data;
							type_name typeNameResult = element_types_names.find(MAKELONG(MAKEWORD(size>3 ? arr[3] : 0, size >2 ? arr[2] : 0), MAKEWORD(size > 1 ? arr[1] : 0, arr[0])))->second;
							seek->elemID = typeNameResult.name;
							//strcpy_s(seek->)
							//memcpy(seek->ID, selement->data, selement->length);
						}
						if (element->children[j]->type == EET::_UNSIGNED)
						{
							auto selement = dynamic_cast<uint_element*>(element->children[j]);
							seek->SeekPosition = selement->data;
						}
					}
					m_masterData->SeekHead.push_back(seek);
					//ReadEbmlElementTree(&pData)
					//				auto test = childs[0].n
					//				children seeks = *reinterpret_cast<children*>(ReadEbmlElementTree(&pData, &cbLen, pAte, childs.elemsize));


					//m_masterData.SeakHead.push_back(Seek(childs[i].typedata))
				}
			}
		}
		/*else if (name == "Void")
		{
			if (type != EET::JUST_GO_ON && type != EET::MASTER)
			{
				auto data = ReadSimpleElement(&pData, &cbLen, pAte, type, size);
			}
		}*/
		else if (name == "Info")
		{
			auto segInfo = new SegmentInformation();
			if (masterElement != nullptr)
			{
				for (int i = 0; i < masterElement->children.size(); ++i)
				{
					if (masterElement->children[i]->type == EET::BINARY)
					{
						auto selement = dynamic_cast<binary_element*>(masterElement->children[i]);
						memcpy(&segInfo->SegmentUID[0], selement->data, selement->length);
					}
					else if (masterElement->children[i]->type == EET::_UNSIGNED)
					{
						auto selement = dynamic_cast<uint_element*>(masterElement->children[i]);
						segInfo->TimecodeScale = selement->data;
					}
					else if (masterElement->children[i]->type == EET::TEXTU)
					{
						auto selement = dynamic_cast<string_element*>(masterElement->children[i]);
						if (strcmp(selement->name, "MuxingApp") == 0)
							segInfo->MuxingApp = selement->data;
						else if (strcmp(selement->name, "WritingApp") == 0)
							segInfo->WritingApp = selement->data;
					}
					else if (masterElement->children[i]->type == EET::_FLOAT)
					{
						auto selement = dynamic_cast<float_element*>(masterElement->children[i]);
						if (strcmp(selement->name, "Duration") == 0)
							segInfo->Duration = selement->data;
						//else if (strcmp(selement->name, "WritingApp") == 0)
						//	segInfo->WritingApp = selement->data;
					}
				}

				m_masterData->SegInfo = segInfo;
			}
					///handler.segment_info = childs;  //non dictionary for some reason
			////handler.segment_info_available();
			////now convert to dict
			

		}
		else if (name == "Tracks")
		{
			if (masterElement != nullptr)
			{
				for (int i = 0; i < masterElement->children.size(); ++i)
				{
					auto base = masterElement->children[i];
					auto element = dynamic_cast<master_element*>(base);
					auto trackEntry = new TrackData();
					for (int j = 0; j < element->children.size(); ++j)
					{
						if (element->children[j]->type == EET::TEXTA)
						{
							auto selement = dynamic_cast<string_element*>(element->children[j]);
							if (strcmp(selement->name, "CodecID") == 0)
								trackEntry->CodecID = selement->data;

						}
						else if (element->children[j]->type == EET::BINARY)
						{
							auto selement = dynamic_cast<binary_element*>(element->children[j]);
							if (strcmp(selement->name, "CodecPrivate") == 0)
							{
								trackEntry->CodecPrivate = selement->data;
								trackEntry->CodecPrivateLength = selement->length;
							}
						}
						else if (element->children[j]->type == EET::_UNSIGNED)
						{
							auto selement = dynamic_cast<uint_element*>(element->children[j]);
							if (strcmp(selement->name, "TrackNumber") == 0)
								trackEntry->TrackNumber = selement->data;
							else if (strcmp(selement->name, "TrackUID") == 0)
								trackEntry->TrackUID = selement->data;
							else if (strcmp(selement->name, "TrackType") == 0)
								trackEntry->TrackType = selement->data;
							else if (strcmp(selement->name, "FlagEnabled") == 0)
								trackEntry->FlagEnabled = selement->data;
							else if (strcmp(selement->name, "FlagDefault") == 0)
								trackEntry->FlagDefault = selement->data;
							else if (strcmp(selement->name, "FlagForced") == 0)
								trackEntry->FlagForced = selement->data;
							else if (strcmp(selement->name, "FlagLacing") == 0)
								trackEntry->FlagLacing = selement->data;
							else if (strcmp(selement->name, "MinCache") == 0)
								trackEntry->MinCache = selement->data;
							else if (strcmp(selement->name, "MaxCache") == 0)
								trackEntry->MaxCache = selement->data;
							else if (strcmp(selement->name, "MaxBlockAdditionID") == 0)
								trackEntry->MaxBlockAdditionID = selement->data;
							else if (strcmp(selement->name, "CodecDecodeAll") == 0)
								trackEntry->CodecDecodeAll = selement->data;
							else if (strcmp(selement->name, "DefaultDuration") == 0)
								trackEntry->DefaultDuration = selement->data;
						}
						else if (element->children[j]->type == EET::MASTER)
						{
							auto selement = dynamic_cast<master_element*>(element->children[j]);
							auto video = new Video();
							auto audio = new Audio();
							for (int k = 0; k < selement->children.size(); ++k)
							{
								if (selement->children[k]->type == EET::_UNSIGNED)
								{
									auto sselement = dynamic_cast<uint_element*>(selement->children[k]);
									if ((strcmp(sselement->name, "PixelWidth") == 0) && (strcmp(selement->name, "Video") == 0))
										video->PixelWidth = sselement->data;
									else if ((strcmp(sselement->name, "PixelHeight") == 0) && (strcmp(selement->name, "Video") == 0))
										video->PixelHeight = sselement->data;
									else if ((strcmp(sselement->name, "FlagInterlaced") == 0) && (strcmp(selement->name, "Video") == 0))
										video->FlagInterlaced = sselement->data;
									else if ((strcmp(sselement->name, "Channels") == 0) && (strcmp(selement->name, "Audio") == 0))
										audio->Channels = sselement->data;
									else if ((strcmp(sselement->name, "BitDepth") == 0) && (strcmp(selement->name, "Audio") == 0))
										audio->BitDepth = sselement->data;
								}
								else if (selement->children[k]->type == EET::_FLOAT)
								{
									auto sselement = dynamic_cast<float_element*>(selement->children[k]);
									if ((strcmp(sselement->name, "SamplingFrequency") == 0) && (strcmp(selement->name, "Audio") == 0))
										audio->SamplingFrequency = sselement->data;
									else if ((strcmp(sselement->name, "OutputSamplingFrequency") == 0) && (strcmp(selement->name, "Audio") == 0))
										audio->OutputSamplingFrequency = sselement->data;
								}
							}
							trackEntry->Video = video;
							trackEntry->Audio = audio;
						}

					}
					m_masterData->Tracks.push_back(trackEntry);
				}
			}
		}
		else if (name == "Cues")
		{
			if (masterElement != nullptr)
			{
				for (int i = 0; i < masterElement->children.size(); ++i)
				{
					auto base = masterElement->children[i];
					auto element = dynamic_cast<master_element*>(base);
					auto cuePoint = new CuePoint();
					for (int j = 0; j < element->children.size(); ++j)
					{
						if (element->children[j]->type == EET::MASTER)
						{
							auto selement = dynamic_cast<master_element*>(element->children[j]);
							auto cueTrackPos = new CueTrackPosition();
							for (int k = 0; k < selement->children.size(); ++k)
							{
								if (selement->children[k]->type == EET::_UNSIGNED)
								{
									auto sselement = dynamic_cast<uint_element*>(selement->children[k]);
									if (strcmp(sselement->name, "CueTrack")==0)
										cueTrackPos->CueTrack = sselement->data;
									else if (strcmp(sselement->name, "CueClusterPosition")==0)
										cueTrackPos->CueClusterPosition = sselement->data;
								}
							}
							cuePoint->CueTrackPositions.push_back(cueTrackPos);
						}
						else if (element->children[j]->type == EET::_UNSIGNED)
						{
							auto selement = dynamic_cast<uint_element*>(element->children[j]);
							cuePoint->CueTime = selement->data;
						}
					}
					m_masterData->Cues.push_back(cuePoint);
				}
			}
		}
		else if (name == "Timecode")
		{
			//auto timecode = ReadFixedLengthNumber(&pData, &cbLen, pAte, type, size);
			m_currentBlockTimeCode = (dynamic_cast<uint_element*>(ReadSimpleElement2(&pData, &cbLen, pAte, type, size)))->data;
		}
		else if (name == "SimpleBlock")
		{
			//auto baseData = ReadSimpleElement2(&pData, &cbLen, pAte, type, size);
			//auto blockData = dynamic_cast<binary_element*>(baseData);
			//pData = pData - blockData->length;
			m_currentStream = *pData - 0x80;
			pData++;
			cbLen--;
			(*pAte)++;
			//auto matroskaNum = ParseMatroskaNumber(&pData, false);
			byte temp[2];// = new byte[2];
			temp[0] = *pData;
			pData++;
			cbLen--;
			(*pAte)++;
			temp[1] = *pData;
			pData++;
			cbLen--;
			(*pAte)++;
			// read timecode
			int64 timeCode=0;
			for (int i = 0; i < 2; i++)
			{
				timeCode = timeCode * 0x100 + temp[i];
			}
			if (temp[0] & 0x80)
			{
				timeCode -= pow(2, 8 * 2);
			}
			//delete temp;
			auto flags = *pData;
			pData++;
			cbLen--;
			(*pAte)++;
			auto keyFrame = (flags & 0x80) == 0x80;
			m_isCurrentKeyFrame = keyFrame;
			auto invisible = (flags & 0x08) == 0x08;
			auto discardable = (flags & 0x01) == 0x01;
			auto laceflags = flags & 0x06;

			//auto blockTimeCode = (m_currentBlockTimeCode + *timeCode)*(m_masterData->SegInfo->TimecodeScale*0.000000001);
			m_currentTimeStamp = (m_currentBlockTimeCode + timeCode);

			//SKIP HEADER REMOVAL HEADERS FOR TRACKS???

			if (laceflags == 0x00) //no lacing
			{
				auto fl = (size - 4);
				m_frameCount++;
				*pCircWrite = fl;
				//*pCircWritePosition = 0;
				pCircWrite++;
				//pCircWritePosition++;
				if ((pCircWrite - &m_circularBuffer[0]) == m_cirBufferLength)
				{
					pCircWrite = &m_circularBuffer[0];
					//pCircWritePosition = &m_circularBufferPosition[0];
				}
				//m_frameSizeQueue.push(fl);
				//Send data to stream!
				m_framesReady = true;

			}
			else  // lacing section
			{
				auto numframes = *pData;
				pData++;
				cbLen--;
				(*pAte)++;
				numframes++;  //number in file is minus 1 so add one

				if (laceflags == 0x02)  //Xiph lacing
				{
					throw ref new Exception(-4, L"xiph lacing not supported");
				}
				else if (laceflags == 0x06) //EBML lacing
				{
					int accumLength = 0;
					int64 framelength = 0;
					int64 lastFramelength = 0;
					int skipBytes = 1;
					byte temp[3];// = new byte[2];
					temp[0] = *pData;
					pData++;
					cbLen--;
					(*pAte)++;
					if (temp[0] & 0x80)
					{
						framelength -= pow(2, 8 * 2);
					}
					else
					{
						skipBytes++;
						temp[1] = *pData;
						pData++;
						cbLen--;
						(*pAte)++;
						if (temp[0] & 0x40)
						{
							framelength = (temp[0] - 0x40) * 0x100 + temp[1];
						}
						else
						{
							skipBytes++;
							temp[2] = *pData;
							pData++;
							cbLen--;
							(*pAte)++;
							if (temp[0] & 0x32)
							{
								framelength = ((temp[0] - 0x32) * 0x10000) + (temp[1] * 0x100) + temp[2];
							}
							else
							{
								throw ref new Exception(-4, L"laced frame size bigger than 3 bytes");
							}
						}
					}
					accumLength += framelength + skipBytes;
					//pData += framelength;
					lastFramelength = framelength;

					*pCircWrite = framelength;
					//*pCircWritePosition = skipBytes;
					m_frameCount++;
					pCircWrite++;
					//pCircWritePosition++;
					//auto d = (pCircWrite - &m_circularBuffer[0]);
					if (((pCircWrite - &m_circularBuffer[0])) == 30)
					{
						pCircWrite = &m_circularBuffer[0];
						//pCircWritePosition = &m_circularBufferPosition[0];
					}


					for (int i = 1; i < numframes-1; ++i)
					{
						skipBytes = 1;
						temp[0] = *pData;
						pData++;
						cbLen--;
						(*pAte)++;
						if (temp[0] & 0x80)
						{
							auto uns = (temp[0] - 0x80);
//							if (uns & 0x40)
//								uns = ~(uns - 0x40-1);
							uns -= pow(2, 7 * 0 + 6) - 1;
							framelength = lastFramelength + uns;
						}
						else
						{
							skipBytes++;
							temp[1] = *pData;
							pData++;
							cbLen--;
							(*pAte)++;
							if (temp[0] & 0x40)
							{
								auto uns = ((temp[0] - 0x40) * 0x100 + temp[1]);
								//if (uns & 0x2000)
								uns -= pow(2, 7 * 1 + 6) - 1;
								framelength = lastFramelength + uns;
							}
							else
							{
								skipBytes++;
								temp[2] = *pData;
								pData++;
								cbLen--;
								(*pAte)++;
								if (temp[0] & 0x32)
								{
									auto uns = ((temp[0] - 0x32) * 0x10000) + (temp[1]*0x100) + temp[2];
//									if (uns & 0x2000)
//										uns = ~(uns - 0x2000-1);
									uns -= pow(2, 7 * 2 + 6) - 1;
									framelength = lastFramelength + uns;
								}
								else
								{
									throw ref new Exception(-4, L"laced frame size bigger than 3 bytes");
								}
							}
						}
						accumLength += framelength + skipBytes;
						lastFramelength = framelength;
						//pData += framelength;

						*pCircWrite = framelength;
						//*pCircWritePosition = skipBytes;
						m_frameCount++;
						pCircWrite++;
						//pCircWritePosition++;
						//auto d = (pCircWrite - &m_circularBuffer[0]);
						if (((pCircWrite - &m_circularBuffer[0])) == 30)
						{
							pCircWrite = &m_circularBuffer[0];
							//pCircWritePosition = &m_circularBufferPosition[0];
						}
						//m_frameSizeQueue.push(fl);

					}

					//last frame
					auto lastFrameLength = size - 4 - accumLength;
					*pCircWrite = framelength;
					m_frameCount++;
					pCircWrite++;
					if (((pCircWrite - &m_circularBuffer[0])) == 30)
					{
						pCircWrite = &m_circularBuffer[0];
					}
					//throw ref new Exception(-4, L"EBML lacing not supported");
				}
				else if (laceflags == 0x04)  //fixed size lacing
				{
					auto fl = (size - 5) / numframes;
					if (numframes > m_cirBufferLength)
						throw ref new Exception(-222, L"circular buffer too small");
					for (int i = 0; i < numframes; ++i)
					{
						*pCircWrite = fl;
						//*pCircWritePosition = 0;  //don't skip any bytes
						m_frameCount++;
						pCircWrite++;
						//pCircWritePosition++;
						//auto d = (pCircWrite - &m_circularBuffer[0]);
						if (((pCircWrite - &m_circularBuffer[0])) == 30)
						{
							pCircWrite = &m_circularBuffer[0];
							//pCircWritePosition = &m_circularBufferPosition[0];
						}//m_frameSizeQueue.push(fl);
					}
				}

				//for (int i = 0; i < lengths.size(); ++i)
				//{
					// send data to stream!
				//}

				m_framesReady = true;

			}
			break;
		}
		else if (name == "BlockGroup")
		{
			auto x = 3;
		}
		else
		{
			//break;
			if (type != EET::JUST_GO_ON && type != EET::MASTER)
			{
				auto dump = ReadSimpleElement2(&pData, &cbLen, pAte, type, size);
				delete dump;
			}
		}

		//handler.ebml_top_element(id,name,type,data);


	}

	//delete masterElement;

	return result;
};


//-------------------------------------------------------------------
// FindNextStartCode
// Looks for the next start code in the buffer.
//
// pData: Pointer to the buffer.
// cbLen: Size of the buffer.
// pAte: Receives the number of bytes *before *the start code.
//
// If no start code is found, the method returns S_FALSE.
//-------------------------------------------------------------------

bool Parser::FindNextStartCode(const BYTE *pData, DWORD cbLen, DWORD *pAte)
{
	bool result = false;

	DWORD cbLeft = cbLen;

	while (cbLeft > 4)
	{
		if ((MAKE_DWORD_HOSTORDER(pData) & 0x00FFFFFF) == 0x00010000)
		{
			result = true;
			break;
		}

		cbLeft -= 4;
		pData += 4;
	}
	*pAte = (cbLen - cbLeft);
	return result;
}


//-------------------------------------------------------------------
// ParsePackHeader
// Parses the start of an MPEG-1 pack.
//-------------------------------------------------------------------

//bool Parser::ParsePackHeader(const BYTE *pData, DWORD cbLen, DWORD *pAte)
//{
//	assert(MAKE_DWORD(pData) == MPEG1_PACK_START_CODE);
//
//	if (cbLen < MPEG1_PACK_HEADER_SIZE)
//	{
//		return false; // Not enough data yet.
//	}
//
//	// Check marker bits
//	if (((pData[4] & 0xF1) != 0x21) ||
//		((pData[6] & 0x01) != 0x01) ||
//		((pData[8] & 0x01) != 0x01) ||
//		((pData[9] & 0x80) != 0x80) ||
//		((pData[11] & 0x01) != 0x01))
//	{
//		ThrowException(MF_E_INVALID_FORMAT);
//	}
//
//
//	// Calculate the SCR.
//	LONGLONG scr = ((pData[8] & 0xFE) >> 1) |
//		((pData[7]) << 7) |
//		((pData[6] & 0xFE) << 14) |
//		((pData[5]) << 22) |
//		((pData[4] & 0x0E) << 29);
//
//	DWORD muxRate = ((pData[11] & 0xFE) >> 1) |
//		((pData[10]) << 7) |
//		((pData[9] & 0x7F) << 15);
//
//
//	m_SCR = scr;
//	m_muxRate = muxRate;
//
//	*pAte = MPEG1_PACK_HEADER_SIZE;
//
//	return true;
//}


//-------------------------------------------------------------------
// ParseSystemHeader.
// Parses the MPEG-1 system header.
//
// NOTES:
// The system header optionally appears after the pack header.
// The first pack must contain a system header.
// Subsequent packs may contain a system header.
//-------------------------------------------------------------------

//bool Parser::ParseSystemHeader(const BYTE *pData, DWORD cbLen, DWORD *pAte)
//{
//	assert(MAKE_DWORD(pData) == MPEG1_SYSTEM_HEADER_CODE);
//
//	if (cbLen < MPEG1_SYSTEM_HEADER_MIN_SIZE)
//	{
//		return false; // Not enough data yet.
//	}
//
//	// Find the total header length.
//	DWORD cbHeaderLen = MPEG1_SYSTEM_HEADER_PREFIX + MAKE_WORD(pData[4], pData[5]);
//
//	if (cbHeaderLen < MPEG1_SYSTEM_HEADER_MIN_SIZE - MPEG1_SYSTEM_HEADER_PREFIX)
//	{
//		ThrowException(MF_E_INVALID_FORMAT);
//	}
//
//	if (cbLen < cbHeaderLen)
//	{
//		return false; // Not enough data yet.
//	}
//
//	// We have enough data to parse the header.
//
//	// Did we already see a system header?
//	if (!HasSystemHeader)
//	{
//		// This is the first time we've seen the header. Parse it.
//
//		// Calculate the number of stream info's in the header.
//		DWORD cStreamInfo = (cbHeaderLen - MPEG1_SYSTEM_HEADER_MIN_SIZE) / MPEG1_SYSTEM_HEADER_STREAM;
//
//		// Calculate the structure size.
//		DWORD cbSize = sizeof(MPEG1SystemHeader);
//		if (cStreamInfo > 1)
//		{
//			cbSize += sizeof(MPEG1StreamHeader) * (cStreamInfo - 1);
//		}
//
//		// Allocate room for the header.
//		m_header = ref new ExpandableStruct<MPEG1SystemHeader>(cbSize);
//		try
//		{
//			m_header->Get()->cbSize = cbSize;
//
//			// Check marker bits
//			if (((pData[6] & 0x80) != 0x80) ||
//				((pData[8] & 0x01) != 0x01) ||
//				((pData[10] & 0x20) != 0x20) ||
//				(pData[11] != 0xFF))
//			{
//				ThrowException(MF_E_INVALID_FORMAT);
//			}
//
//			m_header->Get()->rateBound = ((pData[6] & 0x7F) << 16) | (pData[7] << 8) | (pData[8] >> 1);
//			m_header->Get()->cAudioBound = pData[9] >> 2;
//			m_header->Get()->bFixed = HAS_FLAG(pData[9], 0x02);
//			m_header->Get()->bCSPS = HAS_FLAG(pData[9], 0x01);
//			m_header->Get()->bAudioLock = HAS_FLAG(pData[10], 0x80);
//			m_header->Get()->bVideoLock = HAS_FLAG(pData[10], 0x40);
//			m_header->Get()->cVideoBound = pData[10] & 0x1F;
//			m_header->Get()->cStreams = cStreamInfo;
//
//			// Parse the stream information.
//			const BYTE *pStreamInfo = pData + MPEG1_SYSTEM_HEADER_MIN_SIZE;
//
//			for (DWORD i = 0; i < cStreamInfo; i++)
//			{
//				ParseStreamData(pStreamInfo, m_header->Get()->streams[i]);
//
//				pStreamInfo += MPEG1_SYSTEM_HEADER_STREAM;
//			}
//		}
//		catch (Exception^)
//		{
//			m_header = nullptr;
//			throw;
//		}
//	}
//
//	*pAte = cbHeaderLen;
//
//	return true;
//}

//-------------------------------------------------------------------
// ParsePacketHeader
//
// Parses the packet header.
//
// If the method returns S_OK, then HasPacket() returns true and the
// caller can start parsing the packet.
//-------------------------------------------------------------------

//bool Parser::ParsePacketHeader(const BYTE *pData, DWORD cbLen, DWORD *pAte)
//{
//	if (!HasSystemHeader)
//	{
//		ThrowException(MF_E_INVALIDREQUEST); // We should not get a packet before the first system header.
//	}
//
//	if (cbLen < MPEG1_PACKET_HEADER_MIN_SIZE)
//	{
//		return false; // Not enough data yet.
//	}
//
//	// Before we parse anything else in the packet header, look for the header length.
//	DWORD cbPacketLen = MAKE_WORD(pData[4], pData[5]) + MPEG1_PACKET_HEADER_MIN_SIZE;
//
//	// We want enough data for the maximum packet header OR the total packet size, whichever is less.
//	if (cbLen < cbPacketLen && cbLen < MPEG1_PACKET_HEADER_MAX_SIZE)
//	{
//		return false; // Not enough data yet.
//	}
//
//	// Make sure the start code is 0x000001xx
//	if ((MAKE_DWORD(pData) & 0xFFFFFF00) != MPEG1_START_CODE_PREFIX)
//	{
//		ThrowException(MF_E_INVALID_FORMAT);
//	}
//
//	BYTE id = 0;
//	StreamType type = StreamType_Unknown;
//	BYTE num = 0;
//	bool bHasPTS = false;
//
//	ZeroMemory(&m_curPacketHeader, sizeof(m_curPacketHeader));
//
//	// Find the stream ID.
//	id = pData[3];
//	ParseStreamId(id, &type, &num);
//
//	DWORD cbLeft = cbPacketLen - MPEG1_PACKET_HEADER_MIN_SIZE;
//	pData = pData + MPEG1_PACKET_HEADER_MIN_SIZE;
//	DWORD cbPadding = 0;
//	LONGLONG pts = 0;
//
//	// Go past the stuffing bytes.
//	while ((cbLeft > 0) && (*pData == 0xFF))
//	{
//		AdvanceBufferPointer(pData, cbLeft, 1);
//		++cbPadding;
//	}
//
//	// Check for invalid number of stuffing bytes.
//	if (cbPadding > MPEG1_PACKET_HEADER_MAX_STUFFING_BYTE)
//	{
//		ThrowException(MF_E_INVALID_FORMAT);
//	}
//
//	// The next bits are:
//	// (optional) STD buffer size (2 bytes)
//	// union
//	// {
//	//      PTS (5 bytes)
//	//      PTS + DTS (10 bytes)
//	//      '0000 1111' (1 bytes)
//	// }
//
//	ValidateBufferSize(cbLeft, 1);
//
//	if ((*pData & 0xC0) == 0x40)
//	{
//		// Skip STD buffer size.
//		AdvanceBufferPointer(pData, cbLeft, 2);
//	}
//
//	ValidateBufferSize(cbLeft, 1);
//
//	if ((*pData & 0xF1) == 0x21)
//	{
//		// PTS
//		ValidateBufferSize(cbLeft, 5);
//
//		pts = ParsePTS(pData);
//		bHasPTS = true;
//
//		AdvanceBufferPointer(pData, cbLeft, 5);
//	}
//	else if ((*pData & 0xF1) == 0x31)
//	{
//		// PTS + DTS
//		ValidateBufferSize(cbLeft, 10);
//
//		// Parse PTS but skip DTS.
//		pts = ParsePTS(pData);
//		bHasPTS = true;
//
//		AdvanceBufferPointer(pData, cbLeft, 10);
//	}
//	else if ((*pData) == 0x0F)
//	{
//		AdvanceBufferPointer(pData, cbLeft, 1);
//	}
//	else
//	{
//		ThrowException(MF_E_INVALID_FORMAT); // Unexpected bit field
//	}
//
//	m_curPacketHeader.stream_id = id;
//	m_curPacketHeader.type = type;
//	m_curPacketHeader.number = num;
//	m_curPacketHeader.cbPacketSize = cbPacketLen;
//	m_curPacketHeader.cbPayload = cbLeft;
//	m_curPacketHeader.bHasPTS = bHasPTS;
//	m_curPacketHeader.PTS = pts;
//
//	// Client can read the packet now.
//	m_bHasPacketHeader = true;
//
//	*pAte = cbPacketLen - cbLeft;
//
//	return true;
//}

//-------------------------------------------------------------------
// OnEndOfStream
// Called when the parser reaches the MPEG-1 stop code.
//
// Note: Obviously the parser is not guaranteed to see a stop code
// before the client reaches the end of the source data. The client
// must be prepared to handle that case.
//-------------------------------------------------------------------

void Parser::OnEndOfStream()
{
	m_bEOS = true;
	ClearFrames();
}


//-------------------------------------------------------------------
// Static functions
//-------------------------------------------------------------------


//-------------------------------------------------------------------
// ParsePTS
// Parse the 33-bit Presentation Time Stamp (PTS)
//-------------------------------------------------------------------

LONGLONG ParsePTS(const BYTE *pData)
{
	BYTE byte1 = pData[0];
	WORD word1 = MAKE_WORD(pData[1], pData[2]);
	WORD word2 = MAKE_WORD(pData[3], pData[4]);

	// Check marker bits.
	// The first byte can be '0010xxx1' or '0x11xxxx1'
	if (((byte1 & 0xE1) != 0x21) ||
		((word1 & 0x01) != 0x01) ||
		((word2 & 0x01) != 0x01))
	{
		ThrowException(MF_E_INVALID_FORMAT);
	}

	LARGE_INTEGER li;

	// The PTS is 33 bits, so bit 32 goes in the high-order DWORD
	li.HighPart = (byte1 & 0x08) >> 3;

	li.LowPart = (static_cast<DWORD>(byte1 & 0x06) << 29) |
		(static_cast<DWORD>(word1 & 0xFFFE) << 14) |
		(static_cast<DWORD>(word2) >> 1);

	return li.QuadPart;
}


//-------------------------------------------------------------------
// ParseStreamData
// Parses the stream information (for one stream) in the system
// header.
//-------------------------------------------------------------------

void ParseStreamData(const BYTE *pStreamInfo, MPEG1StreamHeader &header)
{
	// Check marker bits.
	if ((pStreamInfo[1] & 0xC0) != 0xC0)
	{
		ThrowException(MF_E_INVALID_FORMAT); // Invalid bits
	}

	BYTE id = 0;
	BYTE num = 0;
	DWORD bound = 0;
	StreamType type = StreamType_Unknown;

	// The id is a stream code plus (for some types) a stream number, bitwise-OR'd.

	id = pStreamInfo[0];

	ParseStreamId(id, &type, &num);

	// Calculate STD bound.
	bound = pStreamInfo[2] | ((pStreamInfo[1] & 0x1F) << 8);

	if (pStreamInfo[1] & 0x20)
	{
		bound *= 1024;
	}
	else
	{
		bound *= 128;
	}

	header.stream_id = id;
	header.type = type;
	header.number = num;
	header.sizeBound = bound;
}



//-------------------------------------------------------------------
// ParseStreamId
// Parses an MPEG-1 stream ID.
//
// Note:
// The id is a stream code, plus (for some types) a stream number,
// bitwise-OR'd. This function returns the type and the stream number.
//
// See ISO/EIC 11172-1, sec 2.4.4.2
//-------------------------------------------------------------------

void ParseStreamId(BYTE id, StreamType *pType, BYTE *pStreamNum)
{
	StreamType type = StreamType_Unknown;
	BYTE num = 0;

	switch (id)
	{
	case MPEG1_STREAMTYPE_ALL_AUDIO:
		type = StreamType_AllAudio;
		break;

	case MPEG1_STREAMTYPE_ALL_VIDEO:
		type = StreamType_AllVideo;
		break;

	case MPEG1_STREAMTYPE_RESERVED:
		type = StreamType_Reserved;
		break;

	case MPEG1_STREAMTYPE_PRIVATE1:
		type = StreamType_Private1;
		break;

	case MPEG1_STREAMTYPE_PADDING:
		type = StreamType_Padding;
		break;

	case MPEG1_STREAMTYPE_PRIVATE2:
		type = StreamType_Private2;
		break;

	default:
		if ((id & 0xE0) == MPEG1_STREAMTYPE_AUDIO_MASK)
		{
			type = StreamType_Audio;
			num = id & 0x1F;
		}
		else if ((id & 0xF0) == MPEG1_STREAMTYPE_VIDEO_MASK)
		{
			type = StreamType_Video;
			num = id & 0x0F;
		}
		else if ((id & 0xF0) == MPEG1_STREAMTYPE_DATA_MASK)
		{
			type = StreamType_Data;
			num = id & 0x0F;
		}
		else
		{
			ThrowException(MF_E_INVALID_FORMAT); // Unknown stream ID code.
		}
	}

	*pType = type;
	*pStreamNum = num;
}


//-------------------------------------------------------------------
// ReadVideoSequenceHeader
// Parses a video sequence header.
//
// Call Parser::HasPacket() to ensure that pData points to the start
// of a payload, and call Parser::PacketHeader() to verify it is a
// video packet.
//-------------------------------------------------------------------

DWORD ReadVideoSequenceHeader(
	_In_reads_bytes_(cbData) const BYTE *pData,
	DWORD cbData,
	MPEG1VideoSeqHeader &seqHeader
	)
{
	DWORD cbPadding = 0;

	if (pData == nullptr)
	{
		throw ref new InvalidArgumentException();
	}

	// Skip to the sequence header code.
	while ((cbPadding + 4) <= cbData && ((DWORD*)pData)[0] == 0)
	{
		pData += 4;
		cbPadding += 4;
	}

	cbData -= cbPadding;

	// Check for the minimum size buffer.
	if (cbData < MPEG1_VIDEO_SEQ_HEADER_MIN_SIZE)
	{
		return cbPadding;
	}

	// Validate the sequence header code.
	if (MAKE_DWORD(pData) != MPEG1_SEQUENCE_HEADER_CODE)
	{
		ThrowException(MF_E_INVALID_FORMAT);
	}

	// Calculate the actual required size.
	DWORD cbRequired = MPEG1_VIDEO_SEQ_HEADER_MIN_SIZE;

	// Look for quantization matrices.
	if (HAS_FLAG(pData[11], 0x02))
	{
		// Intra quantization matrix is true.
		cbRequired += 64;
	}
	// Intra is false, look for non-intra flag
	else if (HAS_FLAG(pData[11], 0x01))
	{
		cbRequired += 64;
	}

	if (cbData < cbRequired)
	{
		return cbPadding;
	}

	ZeroMemory(&seqHeader, sizeof(seqHeader));

	// Check the marker bit.
	if (!HAS_FLAG(pData[10], 0x20))
	{
		ThrowException(MF_E_INVALID_FORMAT);
	}

	BYTE parCode = pData[7] >> 4;
	BYTE frameRateCode = pData[7] & 0x0F;

	seqHeader.pixelAspectRatio = GetPixelAspectRatio(parCode);
	seqHeader.frameRate = GetFrameRate(frameRateCode);

	seqHeader.width = (pData[4] << 4) | (pData[5] >> 4);
	seqHeader.height = ((pData[5] & 0x0F) << 8) | (pData[6]);
	seqHeader.bitRate = (pData[8] << 10) | (pData[9] << 2) | (pData[10] >> 6);

	if (seqHeader.bitRate == 0)
	{
		ThrowException(MF_E_INVALID_FORMAT); // Not allowed.
	}
	else if (seqHeader.bitRate == 0x3FFFF)
	{
		seqHeader.bitRate = 0; // Variable bit-rate.
	}
	else
	{
		seqHeader.bitRate = seqHeader.bitRate * 400; // Units of 400 bps
	}

	seqHeader.cbVBV_Buffer = (((pData[10] & 0x1F) << 5) | (pData[11] >> 3)) * 2048;
	seqHeader.bConstrained = HAS_FLAG(pData[11], 0x04);

	seqHeader.cbHeader = cbRequired;
	CopyMemory(seqHeader.header, pData, cbRequired);

	return cbRequired + cbPadding;
}



//-------------------------------------------------------------------
// GetFrameRate
// Returns the frame rate from the picture_rate field of the sequence
// header.
//
// See ISO/IEC 11172-2, 2.4.3.2 "Sequence Header"
//-------------------------------------------------------------------

MFRatio GetFrameRate(BYTE frameRateCode)
{
	MFRatio frameRates[] =
	{
		{ 0, 0 },           // invalid
		{ 24000, 1001 },    // 23.976 fps
		{ 24, 1 },
		{ 25, 1 },
		{ 30000, 1001 },    // 29.97 fps
		{ 50, 1 },
		{ 60000, 1001 },    // 59.94 fps
		{ 60, 1 }
	};

	if (frameRateCode < 1 || frameRateCode >= ARRAYSIZE(frameRates))
	{
		ThrowException(MF_E_INVALIDTYPE);
	}

	MFRatio result = { frameRates[frameRateCode].Numerator, frameRates[frameRateCode].Denominator };
	return result;
}

//-------------------------------------------------------------------
// GetPixelAspectRatio
// Returns the pixel aspect ratio (PAR) from the pel_aspect_ratio
// field of the sequence header.
//
// See ISO/IEC 11172-2, 2.4.3.2 "Sequence Header"
//-------------------------------------------------------------------

MFRatio GetPixelAspectRatio(BYTE pixelAspectCode)
{
	DWORD height[] = { 0, 10000, 6735, 7031, 7615, 8055, 8437, 8935, 9157,
		9815, 10255, 10695, 10950, 11575, 12015 };

	const DWORD width = 10000;

	if (pixelAspectCode < 1 || pixelAspectCode >= ARRAYSIZE(height))
	{
		ThrowException(MF_E_INVALIDTYPE);
	}

	MFRatio result = { height[pixelAspectCode], width };
	return result;
}


//-------------------------------------------------------------------
// ReadAudioFrameHeader
// Parses an audio frame header.
//
// Call Parser::HasPacket() to ensure that pData points to the start
// of a payload, and call Parser::PacketHeader() to verify it is an
// audio packet.
//-------------------------------------------------------------------

DWORD ReadAudioFrameHeader(
	const BYTE *pData,
	DWORD cbData,
	MPEG1AudioFrameHeader &audioHeader
	)
{
	MPEG1AudioFrameHeader header;
	ZeroMemory(&header, sizeof(header));

	BYTE bitRateIndex = 0;
	BYTE samplingIndex = 0;

	if (cbData < MPEG1_AUDIO_FRAME_HEADER_SIZE)
	{
		return 0;
	}


	if (pData[0] != 0xFF)
	{
		ThrowException(MF_E_INVALID_FORMAT);
	}

	if (!HAS_FLAG(pData[1], 0xF8))
	{
		ThrowException(MF_E_INVALID_FORMAT);
	}

	// Layer bits
	switch (pData[1] & 0x06)
	{
	case 0x00:
		ThrowException(MF_E_INVALID_FORMAT);

	case 0x06:
		header.layer = MPEG1_Audio_Layer1;
		break;

	case 0x04:
		header.layer = MPEG1_Audio_Layer2;
		break;

	case 0x02:
		header.layer = MPEG1_Audio_Layer3;
		break;

	default:
		ThrowException(E_UNEXPECTED); // Cannot actually happen, given our bitmask above.
	}

	bitRateIndex = (pData[2] & 0xF0) >> 4;
	samplingIndex = (pData[2] & 0x0C) >> 2;

	// Bit rate.
	// Note: Accoring to ISO/IEC 11172-3, some combinations of bitrate and
	// mode are not valid. However, this is up to the decoder to validate.
	header.dwBitRate = GetAudioBitRate(header.layer, bitRateIndex);

	// Sampling frequency.
	header.dwSamplesPerSec = GetSamplingFrequency(samplingIndex);

	header.mode = static_cast<MPEG1AudioMode>((pData[3] & 0xC0) >> 6);
	header.modeExtension = (pData[3] & 0x30) >> 4;
	header.emphasis = (pData[3] & 0x03);

	// Parse the various bit flags.
	if (HAS_FLAG(pData[1], 0x01))
	{
		header.wFlags |= MPEG1_AUDIO_PROTECTION_BIT;
	}
	if (HAS_FLAG(pData[2], 0x01))
	{
		header.wFlags |= MPEG1_AUDIO_PRIVATE_BIT;
	}
	if (HAS_FLAG(pData[3], 0x08))
	{
		header.wFlags |= MPEG1_AUDIO_COPYRIGHT_BIT;
	}
	if (HAS_FLAG(pData[3], 0x04))
	{
		header.wFlags |= MPEG1_AUDIO_ORIGINAL_BIT;
	}

	if (header.mode == MPEG1_Audio_SingleChannel)
	{
		header.nChannels = 1;
	}
	else
	{
		header.nChannels = 2;
	}

	header.nBlockAlign = 1;

	CopyMemory(&audioHeader, &header, sizeof(audioHeader));
	return 4;
};


//-------------------------------------------------------------------
// GetAudioBitRate
// Returns the audio bit rate in KBits per second, from the
// bitrate_index field of the audio frame header.
//
// See ISO/IEC 11172-3, 2.4.2.3, "Header"
//-------------------------------------------------------------------

DWORD GetAudioBitRate(MPEG1AudioLayer layer, BYTE index)
{
	const DWORD MAX_BITRATE_INDEX = 14;

	// Table of bit rates.
	const DWORD bitrate[3][(MAX_BITRATE_INDEX + 1)] =
	{
		{ 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },    // Layer I
		{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },       // Layer II
		{ 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 }         // Layer III
	};

	if (layer < MPEG1_Audio_Layer1 || layer > MPEG1_Audio_Layer3)
	{
		ThrowException(MF_E_INVALID_FORMAT);
	}
	if (index > MAX_BITRATE_INDEX)
	{
		ThrowException(MF_E_INVALID_FORMAT);
	}

	return bitrate[layer][index];
}

//-------------------------------------------------------------------
// GetSamplingFrequency
// Returns the sampling frequency in samples per second, from the
// sampling_frequency field of the audio frame header.
//
// See ISO/IEC 11172-3, 2.4.2.3, "Header"
//-------------------------------------------------------------------

DWORD GetSamplingFrequency(BYTE code)
{
	switch (code)
	{
	case 0:
		return 44100;
	case 1:
		return 48000;
	case 2:
		return 32000;
	default:
		ThrowException(MF_E_INVALID_FORMAT);
	}

	return 0;
}

