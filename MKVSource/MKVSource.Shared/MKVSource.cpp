
#include "pch.h"
#include "MKVSource.h"


#pragma warning( push )
#pragma warning( disable : 4355 )  // 'this' used in base member initializer list

ComPtr<IMFMediaType> CreateVideoMediaType(MKVMasterData* mkvMasterData, int currentTrack);
ComPtr<IMFMediaType> CreateAudioMediaType(MKVMasterData* mkvMasterData, int currentTrack);
ComPtr<IMFMediaType> CreateSubtitleMediaType(MKVMasterData* mkvMasterData, int currentTrack);
void GetStreamMajorType(IMFStreamDescriptor *pSD, GUID *pguidMajorType);


/* Public class methods */

//-------------------------------------------------------------------
// Name: CreateInstance
// Static method to create an instance of the source.
//-------------------------------------------------------------------

ComPtr<MKVSource> MKVSource::CreateInstance()
{
	ComPtr<MKVSource> spSource;

	spSource.Attach(new (std::nothrow) MKVSource());
	if (spSource == nullptr)
	{
		throw ref new OutOfMemoryException();
	}

	return spSource;
}


//-------------------------------------------------------------------
// IUnknown methods
//-------------------------------------------------------------------

HRESULT MKVSource::QueryInterface(REFIID riid, void **ppv)
{
	if (ppv == nullptr)
	{
		return E_POINTER;
	}

	HRESULT hr = E_NOINTERFACE;
	if (riid == IID_IUnknown ||
		riid == IID_IMFMediaEventGenerator ||
		riid == IID_IMFMediaSource ||
		riid == IID_IMFMediaSourceEx)
	{
		(*ppv) = static_cast<IMFMediaSourceEx *>(this);
		AddRef();
		hr = S_OK;
	}
	else if (riid == IID_IMFGetService)
	{
		(*ppv) = static_cast<IMFGetService*>(this);
		AddRef();
		hr = S_OK;
	}
	else if (riid == IID_IMFRateControl)
	{
		(*ppv) = static_cast<IMFRateControl*>(this);
		AddRef();
		hr = S_OK;
	}

	return hr;
}

ULONG MKVSource::AddRef()
{
	return _InterlockedIncrement(&m_cRef);
}

ULONG MKVSource::Release()
{
	LONG cRef = _InterlockedDecrement(&m_cRef);
	if (cRef == 0)
	{
		delete this;
	}
	return cRef;
}

//-------------------------------------------------------------------
// IMFMediaEventGenerator methods
//
// All of the IMFMediaEventGenerator methods do the following:
// 1. Check for shutdown status.
// 2. Call the event queue helper object.
//-------------------------------------------------------------------

HRESULT MKVSource::BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState)
{
	HRESULT hr = S_OK;

	AutoLock lock(m_critSec);

	hr = CheckShutdown();

	if (SUCCEEDED(hr))
	{
		hr = m_spEventQueue->BeginGetEvent(pCallback, punkState);
	}

	return hr;
}

HRESULT MKVSource::EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent)
{
	HRESULT hr = S_OK;

	AutoLock lock(m_critSec);

	hr = CheckShutdown();

	if (SUCCEEDED(hr))
	{
		hr = m_spEventQueue->EndGetEvent(pResult, ppEvent);
	}

	return hr;
}

HRESULT MKVSource::GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent)
{
	// NOTE:
	// GetEvent can block indefinitely, so we don't hold the lock.
	// This requires some juggling with the event queue pointer.

	HRESULT hr = S_OK;

	ComPtr<IMFMediaEventQueue> spQueue;

	{
		AutoLock lock(m_critSec);

		// Check shutdown
		hr = CheckShutdown();

		// Get the pointer to the event queue.
		if (SUCCEEDED(hr))
		{
			spQueue = m_spEventQueue;
		}
	}

	// Now get the event.
	if (SUCCEEDED(hr))
	{
		hr = spQueue->GetEvent(dwFlags, ppEvent);
	}

	return hr;
}

HRESULT MKVSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT *pvValue)
{
	HRESULT hr = S_OK;

	AutoLock lock(m_critSec);

	hr = CheckShutdown();

	if (SUCCEEDED(hr))
	{
		hr = m_spEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
	}

	return hr;
}

//-------------------------------------------------------------------
// IMFMediaSource methods
//-------------------------------------------------------------------


//-------------------------------------------------------------------
// CreatePresentationDescriptor
// Returns a shallow copy of the source's presentation descriptor.
//-------------------------------------------------------------------

HRESULT MKVSource::CreatePresentationDescriptor(
	IMFPresentationDescriptor **ppPresentationDescriptor
	)
{
	if (ppPresentationDescriptor == nullptr)
	{
		return E_POINTER;
	}

	HRESULT hr = S_OK;

	AutoLock lock(m_critSec);

	// Fail if the source is shut down.
	hr = CheckShutdown();

	// Fail if the source was not initialized yet.
	if (SUCCEEDED(hr))
	{
		hr = IsInitialized();
	}

	// Do we have a valid presentation descriptor?
	if (SUCCEEDED(hr))
	{
		if (m_spPresentationDescriptor == nullptr)
		{
			hr = MF_E_NOT_INITIALIZED;
		}
	}

	// Clone our presentation descriptor.
	if (SUCCEEDED(hr))
	{
		hr = m_spPresentationDescriptor->Clone(ppPresentationDescriptor);
	}

	return hr;
}


//-------------------------------------------------------------------
// GetCharacteristics
// Returns capabilities flags.
//-------------------------------------------------------------------

HRESULT MKVSource::GetCharacteristics(DWORD *pdwCharacteristics)
{
	if (pdwCharacteristics == nullptr)
	{
		return E_POINTER;
	}

	HRESULT hr = S_OK;

	AutoLock lock(m_critSec);

	hr = CheckShutdown();

	if (SUCCEEDED(hr))
	{
		*pdwCharacteristics = MFMEDIASOURCE_CAN_PAUSE | MFMEDIASOURCE_CAN_SEEK;
	}

	// NOTE: This sample does not implement seeking, so we do not
	// include the MFMEDIASOURCE_CAN_SEEK flag.
	return hr;
}


//-------------------------------------------------------------------
// Pause
// Pauses the source.
//-------------------------------------------------------------------

HRESULT MKVSource::Pause()
{
	AutoLock lock(m_critSec);

	HRESULT hr = S_OK;

	// Fail if the source is shut down.
	hr = CheckShutdown();

	// Queue the operation.
	if (SUCCEEDED(hr))
	{
		hr = QueueAsyncOperation(SourceOp::OP_PAUSE);
	}

	return hr;
}

//-------------------------------------------------------------------
// Shutdown
// Shuts down the source and releases all resources.
//-------------------------------------------------------------------

HRESULT MKVSource::Shutdown()
{
	AutoLock lock(m_critSec);

	HRESULT hr = S_OK;

	MKVStream *pStream = nullptr;

	hr = CheckShutdown();

	if (SUCCEEDED(hr))
	{
		// Shut down the stream objects.
		for (DWORD i = 0; i < m_streams.GetCount(); i++)
		{
			(void)m_streams[i]->Shutdown();
		}
		// Break circular references with streams here.
		m_streams.Clear();

		// Shut down the event queue.
		if (m_spEventQueue)
		{
			(void)m_spEventQueue->Shutdown();
		}

		// Release objects.

		m_spEventQueue.Reset();
		m_spPresentationDescriptor.Reset();
		m_spByteStream.Reset();
		m_spCurrentOp.Reset();

		delete m_masterData;

		m_parser = nullptr;

		// Set the state.
		m_state = STATE_SHUTDOWN;
	}

	return hr;
}


//-------------------------------------------------------------------
// Start
// Starts or seeks the media source.
//-------------------------------------------------------------------

HRESULT MKVSource::Start(
	IMFPresentationDescriptor *pPresentationDescriptor,
	const GUID *pguidTimeFormat,
	const PROPVARIANT *pvarStartPos
	)
{

	HRESULT hr = S_OK;
	ComPtr<SourceOp> spAsyncOp;

	// Check parameters.

	// Start position and presentation descriptor cannot be nullptr.
	if (pvarStartPos == nullptr || pPresentationDescriptor == nullptr)
	{
		return E_INVALIDARG;
	}

	// Check the time format.
	if ((pguidTimeFormat != nullptr) && (*pguidTimeFormat != GUID_NULL))
	{
		// Unrecognized time format GUID.
		return MF_E_UNSUPPORTED_TIME_FORMAT;
	}

	// Check the data type of the start position.
	if ((pvarStartPos->vt != VT_I8) && (pvarStartPos->vt != VT_EMPTY))
	{
		return MF_E_UNSUPPORTED_TIME_FORMAT;
	}

	AutoLock lock(m_critSec);

	// Check if this is a seek request. This sample does not support seeking.

	if (pvarStartPos->vt == VT_I8)
	{
		//// SEEKING ENABLED SO THIS IS NO LONGER CORRECT
		//// If the current state is STOPPED, then position 0 is valid.
		//// Otherwise, the start position must be VT_EMPTY (current position).

		//if ((m_state != STATE_STOPPED) || (pvarStartPos->hVal.QuadPart != 0))
		//{
		//	hr = MF_E_INVALIDREQUEST;
		//	goto done;
		//}
		if ((m_state != STATE_STOPPED) || (pvarStartPos->hVal.QuadPart != 0))
		{
			/*(void)m_spEventQueue->QueueEventParamVar(
				MESourceSeeked, GUID_NULL, hr, pvarStartPos);*/
		}
	}

	// Fail if the source is shut down.
	hr = CheckShutdown();
	if (FAILED(hr))
	{
		goto done;
	}

	// Fail if the source was not initialized yet.
	hr = IsInitialized();
	if (FAILED(hr))
	{
		goto done;
	}

	// Perform a sanity check on the caller's presentation descriptor.
	hr = ValidatePresentationDescriptor(pPresentationDescriptor);
	if (FAILED(hr))
	{
		goto done;
	}

	// The operation looks OK. Complete the operation asynchronously.

	hr = SourceOp::CreateStartOp(pPresentationDescriptor, &spAsyncOp);
	if (FAILED(hr))
	{
		goto done;
	}

	hr = spAsyncOp->SetData(*pvarStartPos);
	if (FAILED(hr))
	{
		goto done;
	}
	
	

	hr = QueueOperation(spAsyncOp.Get());

done:

	return hr;
}


//-------------------------------------------------------------------
// Stop
// Stops the media source.
//-------------------------------------------------------------------

HRESULT MKVSource::Stop()
{
	AutoLock lock(m_critSec);

	HRESULT hr = S_OK;

	// Fail if the source is shut down.
	hr = CheckShutdown();

	// Fail if the source was not initialized yet.
	if (SUCCEEDED(hr))
	{
		hr = IsInitialized();
	}

	// Queue the operation.
	if (SUCCEEDED(hr))
	{
		hr = QueueAsyncOperation(SourceOp::OP_STOP);
	}

	return hr;
}

//-------------------------------------------------------------------
// IMFMediaSource methods
//-------------------------------------------------------------------

//-------------------------------------------------------------------
// GetService
// Returns a service
//-------------------------------------------------------------------

HRESULT MKVSource::GetService(_In_ REFGUID guidService, _In_ REFIID riid, _Out_opt_ LPVOID *ppvObject)
{
	HRESULT hr = MF_E_UNSUPPORTED_SERVICE;

	if (ppvObject == nullptr)
	{
		return E_POINTER;
	}

	if (guidService == MF_RATE_CONTROL_SERVICE)
	{
		hr = QueryInterface(riid, ppvObject);
	}

	return hr;
}

//-------------------------------------------------------------------
// IMFRateControl methods
//-------------------------------------------------------------------

//-------------------------------------------------------------------
// SetRate
// Sets a rate on the source. Only supported rates are 0 and 1.
//-------------------------------------------------------------------

HRESULT MKVSource::SetRate(BOOL fThin, float flRate)
{
	if (fThin)
	{
		return MF_E_THINNING_UNSUPPORTED;
	}
	if (!IsRateSupported(flRate, &flRate))
	{
		return MF_E_UNSUPPORTED_RATE;
	}

	AutoLock lock(m_critSec);
	HRESULT hr = S_OK;
	SourceOp *pAsyncOp = nullptr;

	if (flRate == m_flRate)
	{
		goto done;
	}

	hr = SourceOp::CreateSetRateOp(fThin, flRate, &pAsyncOp);

	if (SUCCEEDED(hr))
	{
		// Queue asynchronous stop
		hr = QueueOperation(pAsyncOp);
	}

done:
	return hr;
}

//-------------------------------------------------------------------
// GetRate
// Returns a current rate.
//-------------------------------------------------------------------

HRESULT MKVSource::GetRate(_Inout_opt_ BOOL *pfThin, _Inout_opt_ float *pflRate)
{
	if (pfThin == nullptr || pflRate == nullptr)
	{
		return E_INVALIDARG;
	}

	AutoLock lock(m_critSec);
	*pfThin = FALSE;
	*pflRate = m_flRate;

	return S_OK;
}


//-------------------------------------------------------------------
// Public non-interface methods
//-------------------------------------------------------------------

//-------------------------------------------------------------------
// BeginOpen
// Begins reading the byte stream to initialize the source.
// Called by the byte-stream handler when it creates the source.
//
// This method is asynchronous. When the operation completes,
// the callback is invoked and the byte-stream handler calls
// EndOpen.
//
// pStream: Pointer to the byte stream for the MPEG-1 stream.
// pCB: Pointer to the byte-stream handler's callback.
// pState: State object for the async callback. (Can be nullptr.)
//
// Note: The source reads enough data to find one packet header
// for each audio or video stream. This enables the source to
// create a presentation descriptor that describes the format of
// each stream. The source queues the packets that it reads during
// BeginOpen.
//-------------------------------------------------------------------

concurrency::task<void> MKVSource::OpenAsync(IMFByteStream *pStream)
{
	if (pStream == nullptr)
	{
		throw ref new InvalidArgumentException();
	}

	if (m_state != STATE_INVALID)
	{
		ThrowException(MF_E_INVALIDREQUEST);
	}



	DWORD dwCaps = 0;

	AutoLock lock(m_critSec);

	// Create the media event queue.
	ThrowIfError(MFCreateEventQueue(m_spEventQueue.ReleaseAndGetAddressOf()));

	ThrowIfError(MFCreateAttributes(&m_spAttributes, 1));

	// Cache the byte-stream pointer.
	m_spByteStream = pStream;

	// Validate the capabilities of the byte stream.
	// The byte stream must be readable and seekable.
	ThrowIfError(pStream->GetCapabilities(&dwCaps));

	if ((dwCaps & MFBYTESTREAM_IS_SEEKABLE) == 0)
	{
		ThrowException(MF_E_BYTESTREAM_NOT_SEEKABLE);
	}
	else if ((dwCaps & MFBYTESTREAM_IS_READABLE) == 0)
	{
		ThrowException(MF_E_UNSUPPORTED_BYTESTREAM_TYPE);
	}

	// Reserve space in the read buffer.
	m_ReadBuffer = ref new Buffer(INITIAL_BUFFER_SIZE);

	// Create the MPEG-1 parser.
	m_parser = ref new Parser();

	RequestData(READ_SIZE);

	m_state = STATE_OPENING;

	return concurrency::create_task(_openedEvent);
}

//-------------------------------------------------------------------
// OnByteStreamRead
// Called when an asynchronous read completes.
//
// Read requests are issued in the RequestData() method.
//-------------------------------------------------------------------
HRESULT MKVSource::OnByteStreamRead(IMFAsyncResult *pResult)
{
	AutoLock lock(m_critSec);

	DWORD cbRead = 0;

	IUnknown *pState = nullptr;

	if (m_state == STATE_SHUTDOWN)
	{
		// If we are shut down, then we've already released the
		// byte stream. Nothing to do.
		return S_OK;
	}

	try
	{
		// Get the state object. This is either nullptr or the most
		// recent OP_REQUEST_DATA operation.
		(void)pResult->GetState(&pState);

		// Complete the read opertation.
		ThrowIfError(m_spByteStream->EndRead(pResult, &cbRead));

		// If the source stops and restarts in rapid succession, there is
		// a chance this is a "stale" read request, initiated before the
		// stop/restart.

		// To ensure that we don't deliver stale data, we store the
		// OP_REQUEST_DATA operation as a state object in pResult, and compare
		// this against the current value of m_cRestartCounter.

		// If they don't match, we discard the data.

		// NOTE: During BeginOpen, pState is nullptr

		if ((pState == nullptr) || (((SourceOp*)pState)->Data().ulVal == m_cRestartCounter))
		{
			// This data is OK to parse.

			if (cbRead == 0)
			{
				// There is no more data in the stream. Signal end-of-stream.
				EndOfMPEGStream();
			}
			else
			{
				// Update the end-position of the read buffer.
				m_ReadBuffer->MoveEnd(cbRead);

				// Parse the new data.
				ParseData();
			}
		}
	}
	catch (Exception ^exc)
	{
		StreamingError(exc->HResult);
	}

	return S_OK;
}



/* Private methods */

MKVSource::MKVSource() :
OpQueue(m_critSec.m_criticalSection),
m_cRef(1),
m_state(STATE_INVALID),
m_cRestartCounter(0),
m_OnByteStreamRead(this, &MKVSource::OnByteStreamRead),
m_flRate(1.0f)
{
	auto module = ::Microsoft::WRL::GetModuleBase();
	if (module != nullptr)
	{
		module->IncrementObjectCount();
	}
}

MKVSource::~MKVSource()
{
	if (m_state != STATE_SHUTDOWN)
	{
		Shutdown();
	}

	auto module = ::Microsoft::WRL::GetModuleBase();
	if (module != nullptr)
	{
		module->DecrementObjectCount();
	}
}

// IMFMediaSourceEx
STDMETHODIMP MKVSource::GetSourceAttributes(_Outptr_ IMFAttributes **ppAttributes)
{
	if (ppAttributes == nullptr)
	{
		return E_POINTER;
	}

	*ppAttributes = m_spAttributes.Get();
	(*ppAttributes)->AddRef();

	return S_OK;
}

STDMETHODIMP MKVSource::GetStreamAttributes(_In_ DWORD dwStreamIdentifier, _Outptr_ IMFAttributes **ppAttributes)
{
	if (ppAttributes == nullptr)
	{
		return E_POINTER;
	}

	*ppAttributes = m_spAttributes.Get();
	(*ppAttributes)->AddRef();

	return S_OK;
}

STDMETHODIMP MKVSource::SetD3DManager(_In_opt_ IUnknown *pManager)
{
	HRESULT hr = S_OK;
	try
	{
		ThrowIfError(CheckShutdown());
		m_spDeviceManager.Reset();
		if (pManager != nullptr)
		{
			ThrowIfError(pManager->QueryInterface(IID_PPV_ARGS(&m_spDeviceManager)));
		}

		//if (_spStream != nullptr)
		//{
		//	_spStream->SetDXGIDeviceManager(m_spDeviceManager.Get());
		//}
	}
	catch (Exception ^exc)
	{
		hr = exc->HResult;
	}

	return hr;
}


//-------------------------------------------------------------------
// CompleteOpen
//
// Completes the asynchronous BeginOpen operation.
//
// hrStatus: Status of the BeginOpen operation.
//-------------------------------------------------------------------

void MKVSource::CompleteOpen(HRESULT hrStatus)
{
	assert(!_openedEvent._IsTriggered());
	if (FAILED(hrStatus))
	{
		Shutdown();
		_openedEvent.set_exception(ref new COMException(hrStatus));
	}
	else
	{
		_openedEvent.set();
	}
}


//-------------------------------------------------------------------
// IsInitialized:
// Returns S_OK if the source is correctly initialized with an
// MPEG-1 byte stream. Otherwise, returns MF_E_NOT_INITIALIZED.
//-------------------------------------------------------------------

HRESULT MKVSource::IsInitialized() const
{
	if (m_state == STATE_OPENING || m_state == STATE_INVALID)
	{
		return MF_E_NOT_INITIALIZED;
	}
	else
	{
		return S_OK;
	}
}


//-------------------------------------------------------------------
// IsStreamTypeSupported:
// Returns TRUE if the source supports the specified MKV track stream
// type.
//-------------------------------------------------------------------

bool MKVSource::IsStreamTypeSupported(const char* type) const
{
	if (strcmp(type, "und") == 0)
		return false;
	else
		return true;
	// We support video and audio streams.
	//return (type == 0x01 || type == 0x02);  //video or audio
}

//-------------------------------------------------------------------
// IsStreamActive:
// Returns TRUE if the source should deliver a payload, whose type
// is indicated by the specified packet header.
//
// Note: This method does not test the started/paused/stopped state
//       of the source.
//-------------------------------------------------------------------

bool MKVSource::IsStreamActive(const MPEG1PacketHeader &packetHdr)
{
	if (m_state == STATE_OPENING)
	{
		// The source is still opening.
		// Deliver payloads for every supported stream type.
		return true;
		//return IsStreamTypeSupported(packetHdr.type);
	}
	else
	{
		// The source is already opened. Check if the stream is active.
		MKVStream *wpStream = m_streams.Find(m_parser->m_currentStream);

		if (wpStream == nullptr)
		{
			return false;
		}
		else
		{
			return wpStream->IsActive();
		}
	}
}


//-------------------------------------------------------------------
// InitPresentationDescriptor
//
// Creates the source's presentation descriptor, if possible.
//
// During the BeginOpen operation, the source reads packets looking
// for headers for each stream. This enables the source to create the
// presentation descriptor, which describes the stream formats.
//
// This method tests whether the source has seen enough packets
// to create the PD. If so, it invokes the callback to complete
// the BeginOpen operation.
//-------------------------------------------------------------------

void MKVSource::InitPresentationDescriptor()
{
	DWORD cStreams = 0;

	assert(m_spPresentationDescriptor == nullptr);
	assert(m_state == STATE_OPENING);

	m_masterData = m_parser->GetMasterData();
	if (m_masterData == nullptr)
	{
		ThrowException(MF_E_MEDIA_SOURCE_NOT_STARTED);
	}

	// Get the number of streams, as declared in the MPEG-1 header, skipping
	// any streams with an unsupported format.
	for (DWORD i = 0; i < m_masterData->Tracks.size(); i++)
	{
		if (IsStreamTypeSupported(m_masterData->Tracks[i]->CodecID))
		{
			cStreams++;
		}
	}

	// How many streams do we actually have?
	if (cStreams > m_streams.GetCount())
	{
		// Keep reading data until we have seen a packet for each stream.
		return;
	}

	// We should never create a stream we don't support.
	assert(cStreams == m_streams.GetCount());

	// Ready to create the presentation descriptor.

	// Create an array of IMFStreamDescriptor pointers.
	IMFStreamDescriptor **ppSD =
		new (std::nothrow) IMFStreamDescriptor*[cStreams];

	if (ppSD == nullptr)
	{
		throw ref new OutOfMemoryException();
	}

	ZeroMemory(ppSD, cStreams * sizeof(IMFStreamDescriptor*));

	Exception ^error;
	try
	{

		// Fill the array by getting the stream descriptors from the streams.
		for (DWORD i = 0; i < cStreams; i++)
		{
			if (i!=2)
			ThrowIfError(m_streams[i]->GetStreamDescriptor(&ppSD[i]));
		}

		if (cStreams == 3)
			cStreams--;

		// Create the presentation descriptor.
		ThrowIfError(MFCreatePresentationDescriptor(cStreams, ppSD,
			&m_spPresentationDescriptor));

		// Select the first video stream (if any).
		for (DWORD i = 0; i < cStreams; i++)
		{
			GUID majorType = GUID_NULL;

			GetStreamMajorType(ppSD[i], &majorType);
			//if (majorType == MFMediaType_Video)
			if (i==0)
			{
				ThrowIfError(m_spPresentationDescriptor->SelectStream(i));
				//break;
			}
			//if (majorType == MFMediaType_Audio)
			if (i==1)
			{
				ThrowIfError(m_spPresentationDescriptor->SelectStream(i));
				//break;
			}
		}

		//duration of video
		if (m_parser->GetMasterData()->SegInfo->Duration != NULL)
		{
			auto duration = m_parser->GetMasterData()->SegInfo->Duration * 1000*1000*1000*10 / m_parser->GetMasterData()->SegInfo->TimecodeScale;
			ThrowIfError(m_spPresentationDescriptor->SetUINT64(MF_PD_DURATION, duration));
		}

		ThrowIfError(m_spPresentationDescriptor->SetString(MF_PD_MIME_TYPE, L"video/x-matroska"));

		// Switch state from "opening" to stopped.
		m_state = STATE_STOPPED;

		// Invoke the async callback to complete the BeginOpen operation.
		CompleteOpen(S_OK);
	}
	catch (Exception ^exc)
	{
		error = exc;
	}

	if (ppSD != nullptr)
	{
		for (DWORD i = 0; i < cStreams; i++)
		{
			ppSD[i]->Release();
		}
		delete[] ppSD;
	}

	if (error != nullptr)
	{
		throw error;
	}
}


//-------------------------------------------------------------------
// QueueAsyncOperation
// Queue an asynchronous operation.
//
// OpType: Type of operation to queue.
//
// Note: If the SourceOp object requires additional information, call
// OpQueue<SourceOp>::QueueOperation, which takes a SourceOp pointer.
//-------------------------------------------------------------------

HRESULT MKVSource::QueueAsyncOperation(SourceOp::Operation OpType)
{
	HRESULT hr = S_OK;
	ComPtr<SourceOp> spOp;

	hr = SourceOp::CreateOp(OpType, &spOp);

	if (SUCCEEDED(hr))
	{
		hr = QueueOperation(spOp.Get());
	}
	return hr;
}

//-------------------------------------------------------------------
// BeginAsyncOp
//
// Starts an asynchronous operation. Called by the source at the
// begining of any asynchronous operation.
//-------------------------------------------------------------------

void MKVSource::BeginAsyncOp(SourceOp *pOp)
{
	// At this point, the current operation should be nullptr (the
	// previous operation is nullptr) and the new operation (pOp)
	// must not be nullptr.

	if (pOp == nullptr)
	{
		throw ref new InvalidArgumentException();
	}

	if (m_spCurrentOp != nullptr)
	{
		ThrowException(MF_E_INVALIDREQUEST);
	}

	// Store the new operation as the current operation.

	m_spCurrentOp = pOp;
}

//-------------------------------------------------------------------
// CompleteAsyncOp
//
// Completes an asynchronous operation. Called by the source at the
// end of any asynchronous operation.
//-------------------------------------------------------------------

void MKVSource::CompleteAsyncOp(SourceOp *pOp)
{
	HRESULT hr = S_OK;

	// At this point, the current operation (m_spCurrentOp)
	// must match the operation that is ending (pOp).

	if (pOp == nullptr)
	{
		throw ref new InvalidArgumentException();
	}

	if (m_spCurrentOp == nullptr)
	{
		ThrowException(MF_E_INVALIDREQUEST);
	}

	if (m_spCurrentOp.Get() != pOp)
	{
		throw ref new InvalidArgumentException();
	}

	// Release the current operation.
	m_spCurrentOp.Reset();

	// Process the next operation on the queue.
	ThrowIfError(ProcessQueue());
}

//-------------------------------------------------------------------
// DispatchOperation
//
// Performs the asynchronous operation indicated by pOp.
//
// NOTE:
// This method implements the pure-virtual OpQueue::DispatchOperation
// method. It is always called from a work-queue thread.
//-------------------------------------------------------------------

HRESULT MKVSource::DispatchOperation(SourceOp *pOp)
{
	AutoLock lock(m_critSec);

	HRESULT hr = S_OK;

	if (m_state == STATE_SHUTDOWN)
	{
		return S_OK; // Already shut down, ignore the request.
	}

	try
	{
		switch (pOp->Op())
		{
			// IMFMediaSource methods:
		case SourceOp::OP_START:
			DoStart((StartOp*)pOp);
			break;

		case SourceOp::OP_STOP:
			DoStop(pOp);
			break;

		case SourceOp::OP_PAUSE:
			DoPause(pOp);
			break;

		case SourceOp::OP_SETRATE:
			DoSetRate(pOp);
			break;

			// Operations requested by the streams:
		case SourceOp::OP_REQUEST_DATA:
			OnStreamRequestSample(pOp);
			break;

		case SourceOp::OP_END_OF_STREAM:
			OnEndOfStream(pOp);
			break;

		default:
			ThrowException(E_UNEXPECTED);
		}
	}
	catch (Exception ^exc)
	{
		StreamingError(exc->HResult);
	}

	return hr;
}


//-------------------------------------------------------------------
// ValidateOperation
//
// Checks whether the source can perform the operation indicated
// by pOp at this time.
//
// If the source cannot perform the operation now, the method
// returns MF_E_NOTACCEPTING.
//
// NOTE:
// Implements the pure-virtual OpQueue::ValidateOperation method.
//-------------------------------------------------------------------

HRESULT MKVSource::ValidateOperation(SourceOp *pOp)
{
	if (m_spCurrentOp != nullptr)
	{
		ThrowException(MF_E_NOTACCEPTING);
	}
	return S_OK;
}



//-------------------------------------------------------------------
// DoStart
// Perform an async start operation (IMFMediaSource::Start)
//
// pOp: Contains the start parameters.
//
// Note: This sample currently does not implement seeking, and the
// Start() method fails if the caller requests a seek.
//-------------------------------------------------------------------

void MKVSource::DoStart(StartOp *pOp)
{
	assert(pOp->Op() == SourceOp::OP_START);

	BeginAsyncOp(pOp);

	try
	{
		ComPtr<IMFPresentationDescriptor> spPD;

		// Get the presentation descriptor from the SourceOp object.
		// This is the PD that the caller passed into the Start() method.
		// The PD has already been validated.
		ThrowIfError(pOp->GetPresentationDescriptor(&spPD));

		// Because this sample does not support seeking, the start
		// position must be 0 (from stopped) or "current position."

		// If the sample supported seeking, we would need to get the
		// start position from the PROPVARIANT data contained in pOp.
		

		// Select/deselect streams, based on what the caller set in the PD.
		// This method also sends the MENewStream/MEUpdatedStream events.
		SelectStreams(spPD.Get(), pOp->Data());

		m_state = STATE_STARTED;

		auto startPos = pOp->Data().hVal.QuadPart;
		if (startPos != 0)
		{
			m_parser->m_startPosition = *(&pOp->Data());
			// Queue the "seeked" event. The event data is the seeked position.
			ThrowIfError(m_spEventQueue->QueueEventParamVar(
				MESourceSeeked,
				GUID_NULL,
				S_OK,
				&pOp->Data()
				));
		}
		else
		{

			// Queue the "started" event. The event data is the start position.
			ThrowIfError(m_spEventQueue->QueueEventParamVar(
				MESourceStarted,
				GUID_NULL,
				S_OK,
				&pOp->Data()
				));
		}

	}
	catch (Exception ^exc)
	{
		// Failure. Send the error code to the application.

		// Note: It's possible that QueueEvent itself failed, in which case it
		// is likely to fail again. But there is no good way to recover in
		// that case.

		(void)m_spEventQueue->QueueEventParamVar(
			MESourceStarted, GUID_NULL, exc->HResult, nullptr);

		CompleteAsyncOp(pOp);

		throw;
	}

	CompleteAsyncOp(pOp);
}


//-------------------------------------------------------------------
// DoStop
// Perform an async stop operation (IMFMediaSource::Stop)
//-------------------------------------------------------------------

void MKVSource::DoStop(SourceOp *pOp)
{
	BeginAsyncOp(pOp);

	try
	{
		QWORD qwCurrentPosition = 0;

		// Stop the active streams.
		for (DWORD i = 0; i < m_streams.GetCount(); i++)
		{
			if (m_streams[i]->IsActive())
			{
				m_streams[i]->Stop();
			}
		}

		// Seek to the start of the file. If we restart after stopping,
		// we will start from the beginning of the file again.
		ThrowIfError(m_spByteStream->Seek(
			msoBegin,
			0,
			MFBYTESTREAM_SEEK_FLAG_CANCEL_PENDING_IO,
			&qwCurrentPosition
			));

		// Increment the counter that tracks "stale" read requests.
		++m_cRestartCounter; // This counter is allowed to overflow.

		m_spSampleRequest.Reset();

		m_state = STATE_STOPPED;

		// Send the "stopped" event. This might include a failure code.
		(void)m_spEventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
	}
	catch (Exception ^exc)
	{
		m_spSampleRequest.Reset();

		m_state = STATE_STOPPED;

		// Send the "stopped" event. This might include a failure code.
		(void)m_spEventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, exc->HResult, nullptr);

		CompleteAsyncOp(pOp);

		throw;
	}

	CompleteAsyncOp(pOp);
}


//-------------------------------------------------------------------
// DoPause
// Perform an async pause operation (IMFMediaSource::Pause)
//-------------------------------------------------------------------

void MKVSource::DoPause(SourceOp *pOp)
{
	BeginAsyncOp(pOp);

	try
	{
		// Pause is only allowed while running.
		if (m_state != STATE_STARTED)
		{
			ThrowException(MF_E_INVALID_STATE_TRANSITION);
		}

		// Pause the active streams.
		for (DWORD i = 0; i < m_streams.GetCount(); i++)
		{
			if (m_streams[i]->IsActive())
			{
				m_streams[i]->Pause();
			}
		}

		m_state = STATE_PAUSED;

		// Send the "paused" event. This might include a failure code.
		(void)m_spEventQueue->QueueEventParamVar(MESourcePaused, GUID_NULL, S_OK, nullptr);
	}
	catch (Exception ^exc)
	{
		// Send the "paused" event. This might include a failure code.
		(void)m_spEventQueue->QueueEventParamVar(MESourcePaused, GUID_NULL, exc->HResult, nullptr);

		CompleteAsyncOp(pOp);

		throw;
	}

	CompleteAsyncOp(pOp);
}

//-------------------------------------------------------------------
// DoSetRate
// Perform an async set rate operation (IMFRateControl::SetRate)
//-------------------------------------------------------------------

void MKVSource::DoSetRate(SourceOp *pOp)
{
	SetRateOp *pSetRateOp = static_cast<SetRateOp*>(pOp);
	BeginAsyncOp(pOp);

	try
	{
		// Set rate on active streams.
		for (DWORD i = 0; i < m_streams.GetCount(); i++)
		{
			if (m_streams[i]->IsActive())
			{
				m_streams[i]->SetRate(pSetRateOp->GetRate());
			}
		}

		m_flRate = pSetRateOp->GetRate();

		(void)m_spEventQueue->QueueEventParamVar(MESourceRateChanged, GUID_NULL, S_OK, nullptr);
	}
	catch (Exception ^exc)
	{
		// Send the "rate changted" event. This might include a failure code.
		(void)m_spEventQueue->QueueEventParamVar(MESourceRateChanged, GUID_NULL, exc->HResult, nullptr);

		CompleteAsyncOp(pOp);

		throw;
	}

	CompleteAsyncOp(pOp);
}

//-------------------------------------------------------------------
// StreamRequestSample
// Called by streams when they need more data.
//
// Note: This is an async operation. The stream requests more data
// by queueing an OP_REQUEST_DATA operation.
//-------------------------------------------------------------------

void MKVSource::OnStreamRequestSample(SourceOp *pOp)
{
	HRESULT hr = S_OK;

	BeginAsyncOp(pOp);

	// Ignore this request if we are already handling an earlier request.
	// (In that case m_pSampleRequest will be non-nullptr.)

	try
	{
		if (m_spSampleRequest == nullptr)
		{
			// Add the request counter as data to the operation.
			// This counter tracks whether a read request becomes "stale."

			PROPVARIANT var;
			var.vt = VT_UI4;
			var.ulVal = m_cRestartCounter;

			ThrowIfError(pOp->SetData(var));

			// Store this while the request is pending.
			m_spSampleRequest = pOp;

			// Try to parse data - this will invoke a read request if needed.
			ParseData();
		}
	}
	catch (Exception ^exc)
	{
		CompleteAsyncOp(pOp);
		throw;
	}

	CompleteAsyncOp(pOp);
}


//-------------------------------------------------------------------
// OnEndOfStream
// Called by each stream when it sends the last sample in the stream.
//
// Note: When the media source reaches the end of the MPEG-1 stream,
// it calls EndOfStream on each stream object. The streams might have
// data still in their queues. As each stream empties its queue, it
// notifies the source through an async OP_END_OF_STREAM operation.
//
// When every stream notifies the source, the source can send the
// "end-of-presentation" event.
//-------------------------------------------------------------------

void MKVSource::OnEndOfStream(SourceOp *pOp)
{
	BeginAsyncOp(pOp);

	try
	{
		--m_cPendingEOS;
		if (m_cPendingEOS == 0)
		{
			// No more streams. Send the end-of-presentation event.
			ThrowIfError(m_spEventQueue->QueueEventParamVar(MEEndOfPresentation, GUID_NULL, S_OK, nullptr));
		}
	}
	catch (Exception ^exc)
	{
		CompleteAsyncOp(pOp);

		throw;
	}

	CompleteAsyncOp(pOp);
}



//-------------------------------------------------------------------
// SelectStreams
// Called during START operations to select and deselect streams.
//-------------------------------------------------------------------

void MKVSource::SelectStreams(
	IMFPresentationDescriptor *pPD,   // Presentation descriptor.
	const PROPVARIANT varStart        // New start position.
	)
{
	BOOL fSelected = FALSE;
	BOOL fWasSelected = FALSE;
	DWORD   stream_id = 0;

	MKVStream *wpStream = nullptr; // Not add-ref'd

	// Reset the pending EOS count.
	m_cPendingEOS = 0;

	// Loop throught the stream descriptors to find which streams are active.
	for (DWORD i = 0; i < m_streams.GetCount(); i++)
	{
		ComPtr<IMFStreamDescriptor> spSD;

		ThrowIfError(pPD->GetStreamDescriptorByIndex(i, &fSelected, &spSD));
		ThrowIfError(spSD->GetStreamIdentifier(&stream_id));

		wpStream = m_streams.Find((BYTE)stream_id);
		if (wpStream == nullptr)
		{
			ThrowException(E_INVALIDARG);
		}

		// Was the stream active already?
		fWasSelected = wpStream->IsActive();

		// Activate or deactivate the stream.
		wpStream->Activate(!!fSelected);

		if (fSelected)
		{
			m_cPendingEOS++;

			// If the stream was previously selected, send an "updated stream"
			// event. Otherwise, send a "new stream" event.
			MediaEventType met = fWasSelected ? MEUpdatedStream : MENewStream;

			ThrowIfError(m_spEventQueue->QueueEventParamUnk(met, GUID_NULL, S_OK, wpStream));

			// Start the stream. The stream will send the appropriate event.
			wpStream->Start(varStart);
		}
	}
}


//-------------------------------------------------------------------
// RequestData
// Request the next batch of data.
//
// cbRequest: Amount of data to read, in bytes.
//-------------------------------------------------------------------

void MKVSource::RequestData(DWORD cbRequest)
{
	// Reserve a sufficient read buffer.
	m_ReadBuffer->Reserve(cbRequest);

	// Submit the async read request.
	// When it completes, our OnByteStreamRead method will be invoked.

	ThrowIfError(m_spByteStream->BeginRead(
		m_ReadBuffer->DataPtr + m_ReadBuffer->DataSize,
		cbRequest,
		&m_OnByteStreamRead,
		m_spSampleRequest.Get()
		));
}


//-------------------------------------------------------------------
// ParseData
// Parses the next batch of data.
//-------------------------------------------------------------------

void MKVSource::ParseData()
{
	HRESULT hr = S_OK;

	DWORD cbNextRequest = 0;
	bool  fNeedMoreData = false;

	// Keep processing data until
	// (a) All streams have enough samples, or
	// (b) The parser needs more data in the buffer.

	while (StreamsNeedData())
	{
		DWORD cbAte = 0;    // How much data we consumed from the read buffer.

		//if the master data parsing is done, load it up
		if (m_masterData == nullptr && m_parser->HasFinishedParsedData)
		{
			m_masterData = m_parser->GetMasterData();
		}

		if (m_parser->IsEndOfStream)
		{
			// The parser reached the end of the MPEG-1 stream. Notify the streams.
			EndOfMPEGStream();
		}
		else if (m_parser->HasFrames)
		{
			// The parser reached the start of a new block (or block group).
			fNeedMoreData = !ReadPayload(&cbAte, &cbNextRequest);
		}
		else if (m_parser->m_startPosition.hVal.QuadPart > 0)    // FOR SEEKING
		{
			auto jumpToBytePosition = m_parser->FindSeekPoint();
			m_parser->m_jumpFlag = true;
			m_parser->m_jumpTo = jumpToBytePosition;
			//fNeedMoreData = true;
			//auto hr = m_spByteStream->SetCurrentPosition(jumpToBytePosition);
			//m_ReadBuffer->MoveStart(m_ReadBuffer->DataSize);  //dump the rest of the read buffer
			//clear the position
			m_parser->m_startPosition.hVal.QuadPart = 0;
		}
		else
		{
			// Parse more data.
			fNeedMoreData = !m_parser->ParseBytes(m_ReadBuffer->DataPtr, m_ReadBuffer->DataSize, &cbAte);
		}

		if (m_parser->m_jumpFlag)
		{
			m_parser->m_jumpFlag = false;
			auto hr = m_spByteStream->SetCurrentPosition(m_parser->m_jumpTo);
			m_ReadBuffer->MoveStart(m_ReadBuffer->DataSize);
		}
		else
		{
			// Advance the start of the read buffer by the amount consumed.
			m_ReadBuffer->MoveStart(cbAte);
		}

		// case where reached end of file after parsing... need to restart at Cluster data
		if (m_ReadBuffer->DataSize == 0 && !m_parser->HasFinishedParsedData && !fNeedMoreData)
		{
			m_parser->m_isFinishedParsingMaster = true;
			m_masterData = m_parser->GetMasterData();
			auto hr = m_spByteStream->SetCurrentPosition(m_masterData->Cues[0]->CueTrackPositions[0]->CueClusterPosition + m_masterData->SegmentPosition);

			CreateStreams();

			//create stream with presentation descriptor  *maybe here not best place to do it.
			InitPresentationDescriptor();
		}


		// If we need more data, start an async read operation.
		if (fNeedMoreData)
		{
			RequestData(max(READ_SIZE, cbNextRequest));

			// Break from the loop because we need to wait for the async read to complete.
			break;
		}

	}

	// Flag our state. If a stream requests more data while we are waiting for an async
	// read to complete, we can ignore the stream's request, because the request will be
	// dispatched as soon as we get more data.

	if (!fNeedMoreData)
	{
		m_spSampleRequest.Reset();
	}
}

//-------------------------------------------------------------------
// ReadPayload
// Read the next MPEG-1 payload.
//
// When this method is called:
// - The read position has reached the beginning of a payload.
// - We have the packet header, but not necessarily the entire payload.
//-------------------------------------------------------------------

bool MKVSource::ReadPayload(DWORD *pcbAte, DWORD *pcbNextRequest)
{
	assert(m_parser != nullptr);
	assert(m_parser->HasFrames);
	bool fResult = true;
	DWORD cbPayloadRead = 0;
	DWORD cbPayloadUnread = 0;

	m_parser->m_currentFrameSize = *m_parser->pCircRead;
	auto skipBytes = 0;//*m_parser->pCircReadPosition;
	//m_parser->m_currentFrameSize = m_parser->GetFrameSizeQueue.front();

	// At this point, the read buffer might be larger or smaller than the payload.
	// Calculate which portion of the payload has been read.
	if (m_parser->m_currentFrameSize + skipBytes > m_ReadBuffer->DataSize)
	{
		cbPayloadUnread = m_parser->m_currentFrameSize + skipBytes - m_ReadBuffer->DataSize;
	}

	cbPayloadRead = m_parser->m_currentFrameSize + skipBytes - cbPayloadUnread;

	// Do we need to deliver this payload?
	if (!IsStreamActive(MPEG1PacketHeader()))
	{
		QWORD qwCurrentPosition = 0;

		// Skip this payload. Seek past the unread portion of the payload.
		ThrowIfError(m_spByteStream->Seek(
			msoCurrent,
			cbPayloadUnread,
			MFBYTESTREAM_SEEK_FLAG_CANCEL_PENDING_IO,
			&qwCurrentPosition
			));

		// Advance the data buffer to the end of payload, or the portion
		// that has been read.

		*pcbAte = cbPayloadRead;

		// Tell the parser that we are done with this packet.

		m_parser->pCircRead++;
		//m_parser->pCircReadPosition++;
		if ((m_parser->pCircRead - &m_parser->m_circularBuffer[0]) == m_parser->m_cirBufferLength)
		{
			m_parser->pCircRead = &m_parser->m_circularBuffer[0];
			//m_parser->pCircReadPosition = &m_parser->m_circularBufferPosition[0];
		}
		m_parser->m_frameCount--;
		//m_parser->PopFrameSizeQueue();
		//if (m_parser->GetFrameSizeQueue.size() == 0)
		if (m_parser->m_frameCount == 0)
			m_parser->ClearFrames();
	}
	else if (cbPayloadUnread > 0)
	{
		// Some portion of this payload has not been read. Schedule a read.
		*pcbNextRequest = cbPayloadUnread;

		*pcbAte = 0;

		fResult = false; // Need more data.
	}
	else
	{
		// The entire payload is in the data buffer. Deliver the packet.
		DeliverPayload();

		*pcbAte = cbPayloadRead;

		// Tell the parser that we are done with this packet.
		int trackIndex = -1;
		for (int i = 0; i < m_parser->GetMasterData()->Tracks.size(); ++i)
		{
			if (m_parser->GetMasterData()->Tracks[i]->TrackNumber == m_parser->m_currentStream)
				trackIndex = i;
		}
		m_parser->m_currentTimeStamp = m_parser->m_currentTimeStamp + m_parser->GetMasterData()->Tracks[trackIndex]->DefaultDuration / 1000000;

		m_parser->pCircRead++;
		//m_parser->pCircReadPosition++;
		if ((m_parser->pCircRead - &m_parser->m_circularBuffer[0]) == m_parser->m_cirBufferLength)
		{
			m_parser->pCircRead = &m_parser->m_circularBuffer[0];
			//m_parser->pCircReadPosition = &m_parser->m_circularBufferPosition[0];
		}
		m_parser->m_frameCount--;
		if (m_parser->m_frameCount == 0)
			m_parser->ClearFrames();
		/*m_parser->PopFrameSizeQueue();
		if (m_parser->GetFrameSizeQueue.size() == 0)
			m_parser->ClearFrames();*/
	}

	return fResult;
}

//-------------------------------------------------------------------
// EndOfMPEGStream:
// Called when the parser reaches the end of the MPEG1 stream.
//-------------------------------------------------------------------

void MKVSource::EndOfMPEGStream()
{
	// Notify the streams. The streams might have pending samples.
	// When each stream delivers the last sample, it will send the
	// end-of-stream event to the pipeline and then notify the
	// source.

	// When every stream is done, the source sends the end-of-
	// presentation event.

	for (DWORD i = 0; i < m_streams.GetCount(); i++)
	{
		if (m_streams[i]->IsActive())
		{
			m_streams[i]->EndOfStream();
		}
	}
}



//-------------------------------------------------------------------
// StreamsNeedData:
// Returns TRUE if any streams need more data.
//-------------------------------------------------------------------

bool MKVSource::StreamsNeedData() const
{
	bool fNeedData = false;

	switch (m_state)
	{
	case STATE_OPENING:
		// While opening, we always need data (until we get enough
		// to complete the open operation).
		return true;

	case STATE_SHUTDOWN:
		// While shut down, we never need data.
		return false;

	default:
		// If none of the above, ask the streams.
		for (DWORD i = 0; i < m_streams.GetCount(); i++)
		{
			if (m_streams[i]->NeedsData())
			{
				fNeedData = true;
				break;
			}
		}
		return fNeedData;
	}
}


//-------------------------------------------------------------------
// DeliverPayload:
// Delivers an MPEG-1 payload.
//-------------------------------------------------------------------

void MKVSource::DeliverPayload()
{
	// When this method is called, the read buffer contains a complete
	// payload, and the payload belongs to a stream whose type we support.

	assert(m_parser->HasFrames);

	assert(m_parser->m_currentStream != 3);

	//MPEG1PacketHeader packetHdr;
	MKVStream *wpStream = nullptr;   // not AddRef'd

	ComPtr<IMFMediaBuffer> spBuffer;
	ComPtr<IMFSample> spSample;
	BYTE *pData = nullptr;              // Pointer to the IMFMediaBuffer data.

	//packetHdr = m_parser->PacketHeader;
	auto skipBytes = 0;// *m_parser->pCircReadPosition;

	if (m_parser->m_currentFrameSize + skipBytes > m_ReadBuffer->DataSize)
	{
		assert(FALSE);
		ThrowException(E_UNEXPECTED);
	}

	// If we are still opening the file, then we might need to create this stream.
	if (m_state == STATE_OPENING)
	{
		CreateStreams();
		//CreateStream(m_parser->m_currentFrameSize);
	}

	wpStream = m_streams.Find(m_parser->m_currentStream);
	assert(wpStream != nullptr);

	int frameLength;
	int headerSize = 0;
	if (m_parser->m_currentStream == 1)
	{
		const uint8_t m_startCode[] = { 0x00, 0x00, 0x00, 0x01 };
		const char m_endCode[1] = { 0x00 };

		const uint8_t sps[] =
		{ 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x29, 0xac, 0xd9, 0x80, 0x50, 0x05, 0xba, 0x6a, 0x04, 0x04, 0x02, 0x80, 0x00, 0x01, 0xf4, 0x80, 0x00, 0x5d, 0xc0, 0x47, 0x8c, 0x18, 0xcd }; //30
		const uint8_t pps[] =
		{ 0x00, 0x00, 0x00, 0x01, 0x68, 0xe9, 0x78, 0x23, 0x2c, 0x8b };  //10

		//frameLength = m_parser->m_currentFrameSize + 4 + 7 + 4 + 1;
		
		if (!m_parser->m_insertedHeaderYet)
		{
			m_parser->m_insertedHeaderYet = true;

			frameLength = m_parser->m_currentFrameSize + 40;
			// Create a media buffer for the payload.
			ThrowIfError(MFCreateMemoryBuffer(frameLength, &spBuffer));

			ThrowIfError(spBuffer->Lock(&pData, nullptr, nullptr));

			CopyMemory(pData + skipBytes, sps, 30);
			headerSize += 30;
			CopyMemory(pData + skipBytes + headerSize, pps, 10);
			headerSize += 10;

		}
		else
		{
			frameLength = m_parser->m_currentFrameSize;
			// Create a media buffer for the payload.
			ThrowIfError(MFCreateMemoryBuffer(frameLength, &spBuffer));

			ThrowIfError(spBuffer->Lock(&pData, nullptr, nullptr));

		}


		auto p = m_ReadBuffer->DataPtr;
		int nalu_len = 0;
		while (p < (unsigned char *)m_ReadBuffer->DataPtr + frameLength + skipBytes - headerSize)
		{
			nalu_len = (p[0] << 24) + (p[1] << 16) + (p[2] << 8) + p[3];
			p[0] = 0;
			p[1] = 0;
			p[2] = 0;
			p[3] = 1;
		
			p += (nalu_len + 4);
			//CopyMemory(pData, m_startCode, 4);
		}
		CopyMemory(pData + skipBytes + headerSize, m_ReadBuffer->DataPtr, frameLength - headerSize);
		
		//CopyMemory(pData, m_startCode, 4);
		//CopyMemory(pData, sps, 7);
		//CopyMemory(pData + 4 + 7, pps, 4);
		//CopyMemory(pData + 4 + 7 + 4, m_ReadBuffer->DataPtr, m_parser->m_currentFrameSize);
		//CopyMemory(pData + 4 + 7 + 4 + m_parser->m_currentFrameSize, m_endCode, 1);

	}
	else
	{
		frameLength = m_parser->m_currentFrameSize;

		ThrowIfError(MFCreateMemoryBuffer(frameLength, &spBuffer));

		ThrowIfError(spBuffer->Lock(&pData, nullptr, nullptr));
				
		CopyMemory(pData + skipBytes, m_ReadBuffer->DataPtr, m_parser->m_currentFrameSize);
	}
	ThrowIfError(spBuffer->Unlock());

	ThrowIfError(spBuffer->SetCurrentLength(frameLength));

	ThrowIfError(MFCreateSample(&spSample));
	ThrowIfError(spSample->AddBuffer(spBuffer.Get()));

	/*if (packetHdr.bHasPTS)
	{
		LONGLONG hnsStart = packetHdr.PTS * 10000ll / 90ll;

		ThrowIfError(spSample->SetSampleTime(hnsStart));
	}*/
	//if (m_parser->m_isCurrentKeyFrame)
	int trackIndex = -1;
	for (int i = 0; i < m_parser->GetMasterData()->Tracks.size(); ++i)
	{
		if (m_parser->GetMasterData()->Tracks[i]->TrackNumber == m_parser->m_currentStream)
			trackIndex = i;
	}

	ThrowIfError(spSample->SetSampleTime(m_parser->m_currentTimeStamp*10000));  //if in milliseconds, times 10,000 will make it 100-nanosecond units
	ThrowIfError(spSample->SetSampleDuration(m_parser->GetMasterData()->Tracks[trackIndex]->DefaultDuration / 100));  //in nanoseconds, divide by 100 will make it 100-nanosecond units
	ThrowIfError(spSample->SetUINT32(MFSampleExtension_CleanPoint, m_parser->m_isCurrentKeyFrame));

	// Deliver the payload to the stream.
	wpStream->DeliverPayload(spSample.Get());

	// If the open operation is still pending, check if we're done.
	if (m_state == STATE_OPENING)
	{
		InitPresentationDescriptor();
	}
}

void MKVSource::CreateStreams()
{
	//create all the streams at once using mkvmaster data.
	for (int i = 0; i < m_parser->GetMasterData()->Tracks.size(); ++i)
	{
		ComPtr<IMFMediaType> spType;
		ComPtr<IMFStreamDescriptor> spSD;
		ComPtr<MKVStream> spStream;
		ComPtr<IMFMediaTypeHandler> spHandler;

		// Create a media type, based on the packet type (audio/video)
		switch (m_parser->GetMasterData()->Tracks[i]->TrackType)  //1 video, 2 audio, 3 complex, 0x10 logo, 0x11 subtitle, 0x12 buttons, 0x20 control
		{
		case 1:
			// Video: Read the sequence header and use it to create a media type.
			spType = CreateVideoMediaType(m_parser->GetMasterData(), m_parser->GetMasterData()->Tracks[i]->TrackNumber);
			break;

		case 2:
			// Audio: Read the frame header and use it to create a media type.
			spType = CreateAudioMediaType(m_parser->GetMasterData(), m_parser->GetMasterData()->Tracks[i]->TrackNumber);
			break;

		case 17:
			// Subtitle:  
			spType = CreateSubtitleMediaType(m_parser->GetMasterData(), m_parser->GetMasterData()->Tracks[i]->TrackNumber);
			break;

		default:
			assert(false); // If this case occurs, then IsStreamTypeSupported() is wrong.
			ThrowException(E_UNEXPECTED);
		}

		// Create the stream descriptor from the media type.
		ThrowIfError(MFCreateStreamDescriptor(m_parser->GetMasterData()->Tracks[i]->TrackNumber, 1, spType.GetAddressOf(), &spSD));
		//ThrowIfError(MFCreateStreamDescriptor(packetHdr.stream_id, 1, spType.GetAddressOf(), &spSD));
		// Set the default media type on the stream handler.
		ThrowIfError(spSD->GetMediaTypeHandler(&spHandler));
		ThrowIfError(spHandler->SetCurrentMediaType(spType.Get()));

		// Create the new stream.
		spStream.Attach(new (std::nothrow) MKVStream(this, spSD.Get()));
		if (spStream == nullptr)
		{
			throw ref new OutOfMemoryException();
		}
		spStream->Initialize();

		// Add the stream to the array.
		ThrowIfError(m_streams.AddStream(m_parser->GetMasterData()->Tracks[i]->TrackNumber, spStream.Get()));
	}
}

//-------------------------------------------------------------------
// CreateStream:
// Creates a media stream, based on a packet header.
//-------------------------------------------------------------------

void MKVSource::CreateStream(int packetSize)
{
	// We validate the stream type before calling this method.
	//assert(IsStreamTypeSupported(packetHdr.type));


	// First see if the stream already exists.
	if (m_streams.Find(m_parser->m_currentStream) != nullptr)
		//if (m_streams.Find(packetHdr.stream_id) != nullptr)
	{
		// The stream already exists. Nothing to do.
		return;
	}

	DWORD cbAte = 0;
	BYTE *pPayload = nullptr;

	ComPtr<IMFMediaType> spType;
	ComPtr<IMFStreamDescriptor> spSD;
	ComPtr<MKVStream> spStream;
	ComPtr<IMFMediaTypeHandler> spHandler;

	MPEG1VideoSeqHeader videoSeqHdr;
	MPEG1AudioFrameHeader audioFrameHeader;

	// Get a pointer to the start of the payload.
	pPayload = m_ReadBuffer->DataPtr;

	int trackIndex = -1;
	for (int i = 0; i < m_masterData->Tracks.size(); ++i)
	{
		if (m_masterData->Tracks[i]->TrackNumber == m_parser->m_currentStream)
			trackIndex = i;
	}

	// Create a media type, based on the packet type (audio/video)
	switch (m_masterData->Tracks[trackIndex]->TrackType)  //1 video, 2 audio, 3 complex, 0x10 logo, 0x11 subtitle, 0x12 buttons, 0x20 control
//		switch (packetHdr.type)
	{
	case 1:
		// Video: Read the sequence header and use it to create a media type.
		//cbAte = ReadVideoSequenceHeader(pPayload, packetSize, videoSeqHdr);
		spType = CreateVideoMediaType(m_parser->GetMasterData(), m_parser->m_currentStream);
		break;

	case 2:
		// Audio: Read the frame header and use it to create a media type.
		//cbAte = ReadAudioFrameHeader(pPayload, packetSize, audioFrameHeader);
		spType = CreateAudioMediaType(m_parser->GetMasterData(), m_parser->m_currentStream);
		break;

	case 17:
		// Subtitle:  
		spType = CreateSubtitleMediaType(m_parser->GetMasterData(), m_parser->m_currentStream);
		break;

	default:
		assert(false); // If this case occurs, then IsStreamTypeSupported() is wrong.
		ThrowException(E_UNEXPECTED);
	}

	// Create the stream descriptor from the media type.
	ThrowIfError(MFCreateStreamDescriptor(m_parser->m_currentStream, 1, spType.GetAddressOf(), &spSD));
	//ThrowIfError(MFCreateStreamDescriptor(packetHdr.stream_id, 1, spType.GetAddressOf(), &spSD));
	// Set the default media type on the stream handler.
	ThrowIfError(spSD->GetMediaTypeHandler(&spHandler));
	ThrowIfError(spHandler->SetCurrentMediaType(spType.Get()));

	// Create the new stream.
	spStream.Attach(new (std::nothrow) MKVStream(this, spSD.Get()));
	if (spStream == nullptr)
	{
		throw ref new OutOfMemoryException();
	}
	spStream->Initialize();

	// Add the stream to the array.
	ThrowIfError(m_streams.AddStream(m_parser->m_currentStream, spStream.Get()));
	//ThrowIfError(m_streams.AddStream(packetHdr.stream_id, spStream.Get()));
}


//-------------------------------------------------------------------
// ValidatePresentationDescriptor:
// Validates the presentation descriptor that the caller specifies
// in IMFMediaSource::Start().
//
// Note: This method performs a basic sanity check on the PD. It is
// not intended to be a thorough validation.
//-------------------------------------------------------------------

HRESULT MKVSource::ValidatePresentationDescriptor(IMFPresentationDescriptor *pPD)
{
	try
	{
		BOOL fSelected = FALSE;
		DWORD cStreams = 0;

		if (m_masterData == nullptr)
		{
			ThrowException(E_UNEXPECTED);
		}

		// The caller's PD must have the same number of streams as ours.
		ThrowIfError(pPD->GetStreamDescriptorCount(&cStreams));

		if (cStreams != m_masterData->Tracks.size())
		{
		//	ThrowException(E_INVALIDARG);
		}

		// The caller must select at least one stream.
		for (DWORD i = 0; i < cStreams; i++)
		{
			ComPtr<IMFStreamDescriptor> spSD;
			ThrowIfError(pPD->GetStreamDescriptorByIndex(i, &fSelected, &spSD));
			if (fSelected)
			{
				break;
			}
		}

		if (!fSelected)
		{
			throw ref new InvalidArgumentException();
		}
	}
	catch (Exception ^exc)
	{
		return exc->HResult;
	}
	return S_OK;
}


//-------------------------------------------------------------------
// StreamingError:
// Handles an error that occurs duing an asynchronous operation.
//
// hr: Error code of the operation that failed.
//-------------------------------------------------------------------

void MKVSource::StreamingError(HRESULT hr)
{
	if (m_state == STATE_OPENING)
	{
		// An error happened during BeginOpen.
		// Invoke the callback with the status code.

		CompleteOpen(hr);
	}
	else if (m_state != STATE_SHUTDOWN)
	{
		// An error occurred during streaming. Send the MEError event
		// to notify the pipeline.

		QueueEvent(MEError, GUID_NULL, hr, nullptr);
	}
}

bool MKVSource::IsRateSupported(float flRate, float *pflAdjustedRate)
{
	if (flRate < 0.00001f && flRate > -0.00001f)
	{
		*pflAdjustedRate = 0.0f;
		return true;
	}
	else if (flRate < 1.0001f && flRate > 0.9999f)
	{
		*pflAdjustedRate = 1.0f;
		return true;
	}
	return false;
}


/* SourceOp class */


//-------------------------------------------------------------------
// CreateOp
// Static method to create a SourceOp instance.
//
// op: Specifies the async operation.
// ppOp: Receives a pointer to the SourceOp object.
//-------------------------------------------------------------------

HRESULT SourceOp::CreateOp(SourceOp::Operation op, SourceOp **ppOp)
{
	if (ppOp == nullptr)
	{
		return E_POINTER;
	}

	SourceOp *pOp = new (std::nothrow) SourceOp(op);
	if (pOp == nullptr)
	{
		return E_OUTOFMEMORY;
	}
	*ppOp = pOp;

	return S_OK;
}

//-------------------------------------------------------------------
// CreateStartOp:
// Static method to create a SourceOp instance for the Start()
// operation.
//
// pPD: Presentation descriptor from the caller.
// ppOp: Receives a pointer to the SourceOp object.
//-------------------------------------------------------------------

HRESULT SourceOp::CreateStartOp(IMFPresentationDescriptor *pPD, SourceOp **ppOp)
{
	if (ppOp == nullptr)
	{
		return E_POINTER;
	}

	SourceOp *pOp = new (std::nothrow) StartOp(pPD);
	if (pOp == nullptr)
	{
		return E_OUTOFMEMORY;
	}

	*ppOp = pOp;
	return S_OK;
}

//-------------------------------------------------------------------
// CreateSetRateOp:
// Static method to create a SourceOp instance for the SetRate()
// operation.
//
// fThin: TRUE - thinning is on, FALSE otherwise
// flRate: New rate
// ppOp: Receives a pointer to the SourceOp object.
//-------------------------------------------------------------------

HRESULT SourceOp::CreateSetRateOp(BOOL fThin, float flRate, SourceOp **ppOp)
{
	if (ppOp == nullptr)
	{
		return E_POINTER;
	}

	SourceOp *pOp = new (std::nothrow) SetRateOp(fThin, flRate);
	if (pOp == nullptr)
	{
		return E_OUTOFMEMORY;
	}

	*ppOp = pOp;
	return S_OK;
}

ULONG SourceOp::AddRef()
{
	return _InterlockedIncrement(&m_cRef);
}

ULONG SourceOp::Release()
{
	LONG cRef = _InterlockedDecrement(&m_cRef);
	if (cRef == 0)
	{
		delete this;
	}
	return cRef;
}

HRESULT SourceOp::QueryInterface(REFIID riid, void **ppv)
{
	if (ppv == nullptr)
	{
		return E_POINTER;
	}

	HRESULT hr = E_NOINTERFACE;
	if (riid == IID_IUnknown)
	{
		(*ppv) = static_cast<IUnknown *>(this);
		AddRef();
		hr = S_OK;
	}

	return hr;
}

SourceOp::SourceOp(Operation op) : m_cRef(1), m_op(op)
{
	ZeroMemory(&m_data, sizeof(m_data));
}

SourceOp::~SourceOp()
{
	PropVariantClear(&m_data);
}

HRESULT SourceOp::SetData(const PROPVARIANT &var)
{
	return PropVariantCopy(&m_data, &var);
}


StartOp::StartOp(IMFPresentationDescriptor *pPD) : SourceOp(SourceOp::OP_START), m_spPD(pPD)
{
}

StartOp::~StartOp()
{
}


HRESULT StartOp::GetPresentationDescriptor(IMFPresentationDescriptor **ppPD)
{
	if (ppPD == nullptr)
	{
		return E_POINTER;
	}
	if (m_spPD == nullptr)
	{
		return MF_E_INVALIDREQUEST;
	}
	*ppPD = m_spPD.Get();
	(*ppPD)->AddRef();
	return S_OK;
}

SetRateOp::SetRateOp(BOOL fThin, float flRate)
	: SourceOp(SourceOp::OP_SETRATE)
	, m_fThin(fThin)
	, m_flRate(flRate)
{
}

SetRateOp::~SetRateOp()
{
}

/*  Static functions */


//-------------------------------------------------------------------
// CreateVideoMediaType:
// Create a media type from an MPEG-1 video sequence header.
//-------------------------------------------------------------------

ComPtr<IMFMediaType> CreateVideoMediaType(MKVMasterData* mkvMasterData, int currentTrack)
{
	int trackIndex = -1;
	for (int i = 0; i < mkvMasterData->Tracks.size(); ++i)
	{
		if (mkvMasterData->Tracks[i]->TrackNumber == currentTrack)
			trackIndex = i;
	}
	

	ComPtr<IMFMediaType> spType;

	ThrowIfError(MFCreateMediaType(&spType));

	ThrowIfError(spType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));

	if (strcmp(mkvMasterData->Tracks[trackIndex]->CodecID, "V_MPEG4/ISO/AVC") == 0)
		ThrowIfError(spType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
	else if (strcmp(mkvMasterData->Tracks[trackIndex]->CodecID, "V_MS/VFW/FOURCC") == 0)
		ThrowIfError(spType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_MSS2));
	// Format details.

	//// Frame size
	ThrowIfError(MFSetAttributeSize(
		spType.Get(),
		MF_MT_FRAME_SIZE,
		//1280,720
		mkvMasterData->Tracks[trackIndex]->Video->PixelWidth,
		mkvMasterData->Tracks[trackIndex]->Video->PixelHeight
		//videoSeqHdr.width,
		//videoSeqHdr.height
		));

	// Frame rate
	ThrowIfError(MFSetAttributeRatio(
		spType.Get(),
		MF_MT_FRAME_RATE,
		24000,1001
		//mkvMasterData->Tracks[currentTrack]->Video->
		//videoSeqHdr.frameRate.Numerator,
		//videoSeqHdr.frameRate.Denominator
		));

	// Pixel aspect ratio
	ThrowIfError(MFSetAttributeRatio(
		spType.Get(),
		MF_MT_PIXEL_ASPECT_RATIO,
		1,
		1
		//videoSeqHdr.pixelAspectRatio.Numerator,
		//videoSeqHdr.pixelAspectRatio.Denominator
		));

	// Average bit rate
	ThrowIfError(spType->SetUINT32(MF_MT_AVG_BITRATE, 2165000));//videoSeqHdr.bitRate));

	// Interlacing (progressive frames)
	ThrowIfError(spType->SetUINT32(MF_MT_INTERLACE_MODE, (UINT32)MFVideoInterlace_MixedInterlaceOrProgressive));


	//// Sequence header.
	//ThrowIfError(spType->SetBlob(
	//	MF_MT_MPEG_SEQUENCE_HEADER,
	//	videoSeqHdr.header,             // Byte array
	//	videoSeqHdr.cbHeader            // Size
	//	));

	return spType;
}

ComPtr<IMFMediaType> CreateSubtitleMediaType(MKVMasterData* mkvMasterData, int currentTrack)
{
	int trackIndex = -1;
	for (int i = 0; i < mkvMasterData->Tracks.size(); ++i)
	{
		if (mkvMasterData->Tracks[i]->TrackNumber == currentTrack)
			trackIndex = i;
	}

	ComPtr<IMFMediaType> spType;

	ThrowIfError(MFCreateMediaType(&spType));

	ThrowIfError(spType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	ThrowIfError(spType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_AYUV));
	ThrowIfError(spType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
	ThrowIfError(spType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
	//ThrowIfError(spType->SetUINT32(MF_MT_SAMPLE_SIZE, c_cbOutputSampleSize));
	//ThrowIfError(MFSetAttributeSize(spType.Get(), MF_MT_FRAME_SIZE, c_dwOutputImageWidth, c_dwOutputImageHeight));
	//ThrowIfError(MFSetAttributeRatio(spType.Get(), MF_MT_FRAME_RATE, c_dwOutputFrameRateNumerator, c_dwOutputFrameRateDenominator));
	ThrowIfError(spType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	ThrowIfError(MFSetAttributeRatio(spType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

	
	return spType;
}

//-------------------------------------------------------------------
// CreateAudioMediaType:
// Create a media type from an MPEG-1 audio frame header.
//
// Note: This function fills in an MPEG1WAVEFORMAT structure and then
// converts the structure to a Media Foundation media type
// (IMFMediaType). This is somewhat roundabout but it guarantees
// that the type can be converted back to an MPEG1WAVEFORMAT by the
// decoder if need be.
//
// The WAVEFORMATEX portion of the MPEG1WAVEFORMAT structure is
// converted into attributes on the IMFMediaType object. The rest of
// the struct is stored in the MF_MT_USER_DATA attribute.
//-------------------------------------------------------------------

ComPtr<IMFMediaType> CreateAudioMediaType(MKVMasterData* mkvMasterData, int currentTrack)
{
	int trackIndex = -1;
	for (int i = 0; i < mkvMasterData->Tracks.size(); ++i)
	{
		if (mkvMasterData->Tracks[i]->TrackNumber == currentTrack)
			trackIndex = i;
	}


	ComPtr<IMFMediaType> spType;

	ThrowIfError(MFCreateMediaType(&spType));

	ThrowIfError(spType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));

	if (strcmp(mkvMasterData->Tracks[trackIndex]->CodecID, "A_AC3") == 0)
		ThrowIfError(spType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Dolby_AC3));
	else if (strcmp(mkvMasterData->Tracks[trackIndex]->CodecID, "A_AAC") == 0)
		ThrowIfError(spType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
	else if (strcmp(mkvMasterData->Tracks[trackIndex]->CodecID, "A_MPEG/L3") == 0)
		ThrowIfError(spType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3));
	// Format details.

	//// Number of Channels
	ThrowIfError(spType->SetUINT32(
		MF_MT_AUDIO_NUM_CHANNELS,
		//2));
		mkvMasterData->Tracks[trackIndex]->Audio->Channels));

	//// Samples per second
	ThrowIfError(spType->SetUINT32(
		MF_MT_AUDIO_SAMPLES_PER_SECOND,
		mkvMasterData->Tracks[trackIndex]->Audio->SamplingFrequency));

	// Bits per sample
	ThrowIfError(spType->SetUINT32(
		MF_MT_AUDIO_BITS_PER_SAMPLE,
		16));
		//mkvMasterData->Tracks[trackIndex]->Audio->BitDepth));

	// Bit rate
	ThrowIfError(spType->SetUINT32(
		MF_MT_AVG_BITRATE,
		384000));
	//mkvMasterData->Tracks[trackIndex]->Audio->BitDepth));

	if (strcmp(mkvMasterData->Tracks[trackIndex]->CodecID, "A_AAC") == 0){
		ThrowIfError(spType->SetBlob(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, mkvMasterData->Tracks[trackIndex]->CodecPrivate, mkvMasterData->Tracks[trackIndex]->CodecPrivateLength));
		ThrowIfError(spType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0));
	}
	//Don't have block align or Bytes per second
	
	return spType;
}

// Get the major media type from a stream descriptor.
void GetStreamMajorType(IMFStreamDescriptor *pSD, GUID *pguidMajorType)
{
	if (pSD == nullptr || pguidMajorType == nullptr)
	{
		throw ref new InvalidArgumentException();
	}

	ComPtr<IMFMediaTypeHandler> spHandler;

	ThrowIfError(pSD->GetMediaTypeHandler(&spHandler));
	ThrowIfError(spHandler->GetMajorType(pguidMajorType));
}

#pragma warning( pop )
