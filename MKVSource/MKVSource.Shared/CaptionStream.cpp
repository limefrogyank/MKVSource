#include "pch.h"
#include "CaptionStream.h"
#include "VideoBufferLock.h"
#include <initguid.h>
#include <math.h>
#include <wrl\module.h>

namespace
{
	const DWORD c_dwOutputImageHeight = 256;
	const DWORD c_dwOutputImageWidth = 320;
	const DWORD c_cbOutputSamplenNumPixels = c_dwOutputImageWidth * c_dwOutputImageHeight;
	const DWORD c_cbOutputSampleSize = c_cbOutputSamplenNumPixels * 4;
	const DWORD c_dwOutputFrameRateNumerator = 1;
	const DWORD c_dwOutputFrameRateDenominator = 1;
	const LONGLONG c_llOutputFrameDuration = 1000000ll;
}

class CaptionStream::CSourceLock
{
public:
	_Acquires_lock_(m_spSource)
		CSourceLock(MKVSource *pSource)
		: m_spSource(pSource)
	{
		if (m_spSource)
		{
			m_spSource->Lock();
		}
	}

	_Releases_lock_(m_spSource)
		~CSourceLock()
	{
		if (m_spSource)
		{
			m_spSource->Unlock();
		}
	}

private:
	ComPtr<MKVSource> m_spSource;
};

BYTE Clip(int i)
{
	return (i > 255 ? 255 : (i < 0 ? 0 : i));
}

DWORD YUVToRGB(BYTE Y, BYTE U, BYTE V)
{
	int C = INT(Y) - 16;
	int D = INT(U) - 128;
	int E = INT(V) - 128;

	INT R = (298 * C + 409 * E + 128) >> 8;
	INT G = (298 * C - 100 * D - 208 * E + 128) >> 8;
	INT B = (298 * C + 516 * D + 128) >> 8;

	DWORD ret = 0xff000000;
	BYTE *bCols = reinterpret_cast<BYTE*>(&ret);

	bCols[2] = Clip(R);
	bCols[1] = Clip(G);
	bCols[0] = Clip(B);

	return ret;
}

ref class CFrameGenerator abstract
{
internal:
	CFrameGenerator() { }

	void PrepareFrame(BYTE *pBuf, LONGLONG llTimestamp, LONG lPitch)
	{
		ZeroMemory(pBuf, lPitch * c_dwOutputImageHeight);

		sin((llTimestamp / 100000.0)*3.1415);
		DrawFrame(pBuf, YUVToRGB(128, 128 + BYTE(127 * sin(llTimestamp / 10000000.0)), 128 + BYTE(127 * cos(llTimestamp / 3300000.0))), lPitch);
	}

protected private:
	virtual void DrawFrame(BYTE *pBuf, DWORD dwColor, LONG lPitch) = 0;

	void SetColor(_In_reads_bytes_(cElements) DWORD *pBuf, DWORD dwColor, DWORD cElements)
	{
		for (DWORD nIndex = 0; nIndex < cElements; ++nIndex)
		{
			pBuf[nIndex] = dwColor;
		}
	}
};

ref class CSquareDrawer sealed : public CFrameGenerator
{
protected private:
	void DrawFrame(BYTE *pBuf, DWORD dwColor, LONG lPitch) override
	{
		const DWORD dwDimension = min(c_dwOutputImageWidth, c_dwOutputImageHeight);
		const int nFirstLine = (c_dwOutputImageHeight - dwDimension) / 2;
		const int nStartPos = (c_dwOutputImageWidth - dwDimension) / 2;

		for (int nLine = 0; nLine < dwDimension; ++nLine, pBuf += lPitch)
		{
			DWORD *pLine = reinterpret_cast<DWORD *>(pBuf)+nStartPos;
			SetColor(pLine, dwColor, dwDimension);
		}
	}
};

ref class CCircleDrawer sealed : public CFrameGenerator
{
protected private:
	void DrawFrame(BYTE *pBuf, DWORD dwColor, LONG lPitch) override
	{
		const int dwDimension = min(c_dwOutputImageWidth, c_dwOutputImageHeight);
		const int dwRadius = dwDimension / 2;
		const int nFirstLine = (c_dwOutputImageHeight - dwDimension) / 2;
		const int nStartPos = (c_dwOutputImageWidth - dwDimension) / 2;

		for (int nLine = -dwRadius; nLine < dwRadius; ++nLine, pBuf += lPitch)
		{
			const int nXPos = (int)sqrt(dwRadius*(double)dwRadius - nLine*(double)nLine);
			const int nStartPos = (c_dwOutputImageWidth / 2) - nXPos;
			const int cPixels = nXPos * 2;
			DWORD *pLine = reinterpret_cast<DWORD *>(pBuf)+nStartPos;

			SetColor(pLine, dwColor, cPixels);
		}
	}
};

ref class CTriangleDrawer sealed : public CFrameGenerator
{
protected private:
	void DrawFrame(BYTE *pBuf, DWORD dwColor, LONG lPitch) override
	{
		const DWORD dwDimension = min(c_dwOutputImageWidth, c_dwOutputImageHeight);

		const int nFirstLine = (c_dwOutputImageHeight - dwDimension) / 2;
		const int nStartPos = (c_dwOutputImageWidth - dwDimension) / 2;

		int nLeft = c_dwOutputImageWidth / 2;
		int nRight = nLeft + 1;
		const int cLinesPerPixel = 2;

		for (int nLine = 0, nLinesToGrow = 1; nLine < dwDimension; ++nLine, pBuf += lPitch, --nLinesToGrow)
		{
			if (nLinesToGrow == 0)
			{
				nLinesToGrow = cLinesPerPixel;
				--nLeft;
				++nRight;
			}

			DWORD *pLine = reinterpret_cast<DWORD *>(pBuf)+nLeft;
			SetColor(pLine, dwColor, nRight - nLeft);
		}
	}
};

//CFrameGenerator ^CreateFrameGenerator()
//{
//	switch (eShape)
//	{
//	case GeometricShape_Square:
//	{
//		return ref new CSquareDrawer();
//	}
//	case GeometricShape_Circle:
//	{
//		return ref new CCircleDrawer();
//	}
//	case GeometricShape_Triangle:
//	{
//		return ref new CTriangleDrawer();
//	}
//	default:
//	{
//		throw ref new InvalidArgumentException();
//	}
//	}
//}

//ComPtr<CaptionStream> CaptionStream::CreateInstance(CGeometricMediaSource *pSource, GeometricShape eShape)
//{
//	if (pSource == nullptr)
//	{
//		throw ref new InvalidArgumentException();
//	}
//
//	ComPtr<CaptionStream> spStream;
//	spStream.Attach(new(std::nothrow) CaptionStream(pSource, eShape));
//	if (spStream == nullptr)
//	{
//		throw ref new OutOfMemoryException();
//	}
//
//	spStream->Initialize();
//
//	return spStream;
//}

CaptionStream::CaptionStream(MKVSource *pSource, IMFStreamDescriptor *pSD)
	: m_cRef(1)
	, m_spSource(pSource)
	, m_state(STATE_STOPPED)
	, m_flRate(1.0f)
	, m_spStreamDescriptor(pSD)
{
	auto module = ::Microsoft::WRL::GetModuleBase();
	if (module != nullptr)
	{
		module->IncrementObjectCount();
	}

	assert(pSource != nullptr);
	assert(pSD != nullptr);
}


CaptionStream::~CaptionStream(void)
{
	assert(m_state == STATE_SHUTDOWN);

	auto module = ::Microsoft::WRL::GetModuleBase();
	if (module != nullptr)
	{
		module->DecrementObjectCount();
	}
}

// IUnknown methods

IFACEMETHODIMP CaptionStream::QueryInterface(REFIID riid, void **ppv)
{
	if (ppv == nullptr)
	{
		return E_POINTER;
	}
	HRESULT hr = E_NOINTERFACE;
	(*ppv) = nullptr;
	if (riid == IID_IUnknown ||
		riid == IID_IMFMediaEventGenerator ||
		riid == IID_IMFMediaStream)
	{
		(*ppv) = static_cast<IMFMediaStream *>(this);
		AddRef();
		hr = S_OK;
	}

	return hr;
}

IFACEMETHODIMP_(ULONG) CaptionStream::AddRef()
{
	return InterlockedIncrement(&m_cRef);
}

IFACEMETHODIMP_(ULONG) CaptionStream::Release()
{
	long cRef = InterlockedDecrement(&m_cRef);
	if (cRef == 0)
	{
		delete this;
	}
	return cRef;
}

// IMFMediaEventGenerator methods.
// Note: These methods call through to the event queue helper object.

IFACEMETHODIMP CaptionStream::BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState)
{
	HRESULT hr = S_OK;

	CSourceLock lock(m_spSource.Get());

	hr = CheckShutdown();

	if (SUCCEEDED(hr))
	{
		hr = m_spEventQueue->BeginGetEvent(pCallback, punkState);
	}

	return hr;
}

IFACEMETHODIMP CaptionStream::EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent)
{
	HRESULT hr = S_OK;

	CSourceLock lock(m_spSource.Get());

	hr = CheckShutdown();

	if (SUCCEEDED(hr))
	{
		hr = m_spEventQueue->EndGetEvent(pResult, ppEvent);
	}

	return hr;
}

IFACEMETHODIMP CaptionStream::GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent)
{
	// NOTE:
	// GetEvent can block indefinitely, so we don't hold the lock.
	// This requires some juggling with the event queue pointer.

	HRESULT hr = S_OK;

	ComPtr<IMFMediaEventQueue> spQueue;

	{
		CSourceLock lock(m_spSource.Get());

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

IFACEMETHODIMP CaptionStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, PROPVARIANT const *pvValue)
{
	HRESULT hr = S_OK;

	CSourceLock lock(m_spSource.Get());

	hr = CheckShutdown();

	if (SUCCEEDED(hr))
	{
		hr = m_spEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
	}

	return hr;
}

// IMFMediaStream methods

IFACEMETHODIMP CaptionStream::GetMediaSource(IMFMediaSource **ppMediaSource)
{
	if (ppMediaSource == nullptr)
	{
		return E_INVALIDARG;
	}

	HRESULT hr = S_OK;

	CSourceLock lock(m_spSource.Get());

	hr = CheckShutdown();

	if (SUCCEEDED(hr))
	{
		*ppMediaSource = m_spSource.Get();
		(*ppMediaSource)->AddRef();
	}

	return hr;
}

IFACEMETHODIMP CaptionStream::GetStreamDescriptor(IMFStreamDescriptor **ppStreamDescriptor)
{
	if (ppStreamDescriptor == nullptr)
	{
		return E_INVALIDARG;
	}

	HRESULT hr = S_OK;

	CSourceLock lock(m_spSource.Get());

	hr = CheckShutdown();

	if (SUCCEEDED(hr))
	{
		*ppStreamDescriptor = m_spStreamDescriptor.Get();
		(*ppStreamDescriptor)->AddRef();
	}

	return hr;
}

IFACEMETHODIMP CaptionStream::RequestSample(IUnknown *pToken)
{
	HRESULT hr = S_OK;

	CSourceLock lock(m_spSource.Get());

	try
	{
		ThrowIfError(CheckShutdown());

		if (m_state != STATE_STARTED)
		{
			// We cannot be asked for a sample unless we are in started state
			ThrowException(MF_E_INVALIDREQUEST);
		}

		// Trigger sample delivery
		DeliverSample(pToken);
	}
	catch (Exception ^exc)
	{
		hr = HandleError(exc->HResult);
	}

	return hr;
}

HRESULT CaptionStream::Start()
{
	HRESULT hr = S_OK;

	try
	{
		ThrowIfError(CheckShutdown());

		if (m_state == STATE_STOPPED ||
			m_state == STATE_STARTED)
		{
			if (m_state == STATE_STOPPED)
			{
				m_llCurrentTimestamp = 0;
			}
			m_state = STATE_STARTED;

			// Inform the client that we've started
			ThrowIfError(QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr));
		}
		else
		{
			ThrowException(MF_E_INVALID_STATE_TRANSITION);
		}
	}
	catch (Exception ^exc)
	{
		hr = HandleError(exc->HResult);
	}

	return hr;
}

HRESULT CaptionStream::Stop()
{
	HRESULT hr = S_OK;

	try
	{
		ThrowIfError(CheckShutdown());

		if (m_state == STATE_STARTED)
		{
			m_state = STATE_STOPPED;
			// Inform the client that we've stopped.
			ThrowIfError(QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr));
		}
		else
		{
			hr = MF_E_INVALID_STATE_TRANSITION;
		}
	}
	catch (Exception ^exc)
	{
		hr = HandleError(exc->HResult);
	}

	return hr;
}

HRESULT CaptionStream::SetRate(float flRate)
{
	HRESULT hr = S_OK;

	try
	{
		ThrowIfError(CheckShutdown());

		m_flRate = flRate;
	}
	catch (Exception ^exc)
	{
		hr = HandleError(exc->HResult);
	}

	return hr;
}

void CaptionStream::Shutdown()
{
	try
	{
		ThrowIfError(CheckShutdown());

		if (m_spEventQueue)
		{
			m_spEventQueue->Shutdown();
			m_spEventQueue.Reset();
		}

		if (m_spAllocEx != nullptr)
		{
			m_spAllocEx->UninitializeSampleAllocator();
		}

		m_spStreamDescriptor.Reset();
		m_spDeviceManager.Reset();
		m_state = STATE_SHUTDOWN;

		m_frameGenerator = nullptr;
	}
	catch (Exception ^exc)
	{

	}
}

void CaptionStream::SetDXGIDeviceManager(IMFDXGIDeviceManager *pManager)
{
	m_spDeviceManager = pManager;

	if (m_spDeviceManager)
	{
		CreateVideoSampleAllocator();
	}
}

void CaptionStream::Initialize()
{
	// Create the media event queue.
	ThrowIfError(MFCreateEventQueue(&m_spEventQueue));

	ComPtr<IMFStreamDescriptor> spSD;
	ComPtr<IMFMediaTypeHandler> spMediaTypeHandler;

	// Create a media type object.
	m_spMediaType = CreateMediaType();

	// Now we can create MF stream descriptor.
	ThrowIfError(MFCreateStreamDescriptor(c_dwGeometricStreamId, 1, m_spMediaType.GetAddressOf(), &spSD));
	ThrowIfError(spSD->GetMediaTypeHandler(&spMediaTypeHandler));
	// Set current media type
	ThrowIfError(spMediaTypeHandler->SetCurrentMediaType(m_spMediaType.Get()));
	m_spStreamDescriptor = spSD;
	// State of the stream is started.
	m_state = STATE_STOPPED;

	//m_frameGenerator = CreateFrameGenerator(_eShape);
}

ComPtr<IMFMediaType> CaptionStream::CreateMediaType()
{
	ComPtr<IMFMediaType> spOutputType;
	ThrowIfError(MFCreateMediaType(&spOutputType));

	ThrowIfError(spOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	ThrowIfError(spOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32));
	ThrowIfError(spOutputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
	ThrowIfError(spOutputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
	ThrowIfError(spOutputType->SetUINT32(MF_MT_SAMPLE_SIZE, c_cbOutputSampleSize));
	ThrowIfError(MFSetAttributeSize(spOutputType.Get(), MF_MT_FRAME_SIZE, c_dwOutputImageWidth, c_dwOutputImageHeight));
	ThrowIfError(MFSetAttributeRatio(spOutputType.Get(), MF_MT_FRAME_RATE, c_dwOutputFrameRateNumerator, c_dwOutputFrameRateDenominator));
	ThrowIfError(spOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	ThrowIfError(MFSetAttributeRatio(spOutputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

	return spOutputType;
}

void CaptionStream::DeliverSample(IUnknown *pToken)
{
	ComPtr<IMFSample> spSample = CreateImage();

	ThrowIfError(spSample->SetSampleTime(m_llCurrentTimestamp));
	m_llCurrentTimestamp += c_llOutputFrameDuration;
	ThrowIfError(spSample->SetSampleDuration(c_llOutputFrameDuration));
	// If token was not null set the sample attribute.
	ThrowIfError(spSample->SetUnknown(MFSampleExtension_Token, pToken));
	// Send a sample event.
	ThrowIfError(m_spEventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, spSample.Get()));
}

ComPtr<IMFSample> CaptionStream::CreateImage()
{
	ComPtr<IMFMediaBuffer> spOutputBuffer;
	ComPtr<IMFSample> spSample;
	const LONG pitch = c_dwOutputImageWidth * sizeof(DWORD);

	if (m_frameGenerator == nullptr)
	{
		ThrowException(E_UNEXPECTED);
	}

	if (m_spDeviceManager != nullptr)
	{
		ThrowIfError(m_spAllocEx->AllocateSample(&spSample));
		ThrowIfError(spSample->GetBufferByIndex(0, &spOutputBuffer));
	}
	else
	{
		ThrowIfError(MFCreateMemoryBuffer(c_cbOutputSampleSize, &spOutputBuffer));
		ThrowIfError(MFCreateSample(&spSample));
		ThrowIfError(spSample->AddBuffer(spOutputBuffer.Get()));
	}

	VideoBufferLock lock(spOutputBuffer.Get(), MF2DBuffer_LockFlags_Write, c_dwOutputImageHeight, pitch);

	m_frameGenerator->PrepareFrame(lock.GetData(), m_llCurrentTimestamp, lock.GetStride());

	ThrowIfError(spOutputBuffer->SetCurrentLength(c_cbOutputSampleSize));

	return spSample;
}

HRESULT CaptionStream::HandleError(HRESULT hErrorCode)
{
	if (hErrorCode != MF_E_SHUTDOWN)
	{
		// Send MEError to the client
		hErrorCode = QueueEvent(MEError, GUID_NULL, hErrorCode, nullptr);
	}

	return hErrorCode;
}

void CaptionStream::CreateVideoSampleAllocator()
{
	ComPtr<IMFAttributes> spAttributes;

	ThrowIfError(MFCreateAttributes(&spAttributes, 1));

	ThrowIfError(spAttributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_SHADER_RESOURCE));
	ThrowIfError(MFCreateVideoSampleAllocatorEx(IID_IMFVideoSampleAllocatorEx, (void**)&m_spAllocEx));
	ThrowIfError(m_spAllocEx->SetDirectXManager(m_spDeviceManager.Get()));
	ThrowIfError(m_spAllocEx->InitializeSampleAllocatorEx(1, 4, spAttributes.Get(), m_spMediaType.Get()));
}
