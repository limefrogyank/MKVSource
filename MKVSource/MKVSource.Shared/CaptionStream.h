//// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
//// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
//// PARTICULAR PURPOSE.
////
//// Copyright (c) Microsoft Corporation. All rights reserved

#pragma once
#include "MKVSource.h"

const DWORD c_dwGeometricStreamId = 1;

ref class CFrameGenerator;

class CaptionStream WrlSealed
	: public IMFMediaStream
{
public:
	//static ComPtr<CaptionStream> CreateInstance(MKVSource *pSource, GeometricShape eShape);

	// IUnknown
	IFACEMETHOD(QueryInterface) (REFIID iid, void **ppv);
	IFACEMETHOD_(ULONG, AddRef) ();
	IFACEMETHOD_(ULONG, Release) ();

	// IMFMediaEventGenerator
	IFACEMETHOD(BeginGetEvent) (IMFAsyncCallback *pCallback, IUnknown *punkState);
	IFACEMETHOD(EndGetEvent) (IMFAsyncResult *pResult, IMFMediaEvent **ppEvent);
	IFACEMETHOD(GetEvent) (DWORD dwFlags, IMFMediaEvent **ppEvent);
	IFACEMETHOD(QueueEvent) (MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT *pvValue);

	// IMFMediaStream
	IFACEMETHOD(GetMediaSource) (IMFMediaSource **ppMediaSource);
	IFACEMETHOD(GetStreamDescriptor) (IMFStreamDescriptor **ppStreamDescriptor);
	IFACEMETHOD(RequestSample) (IUnknown *pToken);

	// Other public methods
	HRESULT Start();
	HRESULT Stop();
	HRESULT SetRate(float flRate);
	void Shutdown();
	void SetDXGIDeviceManager(IMFDXGIDeviceManager *pManager);

protected:
	CaptionStream();
	CaptionStream(MKVSource *pSource, IMFStreamDescriptor *pSD);
	~CaptionStream(void);

private:
	class CSourceLock;

private:
	void Initialize();
	ComPtr<IMFMediaType> CreateMediaType();
	void DeliverSample(IUnknown *pToken);
	ComPtr<IMFSample> CreateImage();
	HRESULT HandleError(HRESULT hErrorCode);
	void CreateVideoSampleAllocator();

	HRESULT CheckShutdown() const
	{
		return (m_state == STATE_SHUTDOWN ? MF_E_SHUTDOWN : S_OK);
	}

private:
	long                        m_cRef;                      // reference count
	SourceState                 m_state;					 // Flag to indicate if Shutdown() method was called.
	ComPtr<MKVSource>			m_spSource;
	ComPtr<IMFMediaEventQueue>  m_spEventQueue;              // Event queue
	ComPtr<IMFStreamDescriptor> m_spStreamDescriptor;        // Stream descriptor
	ComPtr<IMFMediaBuffer>      m_spPicture;
	LONGLONG                    m_llCurrentTimestamp;
	ComPtr<IMFDXGIDeviceManager> m_spDeviceManager;
	//GeometricShape              _eShape;
	ComPtr<IMFMediaType>        m_spMediaType;
	ComPtr<IMFVideoSampleAllocatorEx> m_spAllocEx;
	CFrameGenerator^            m_frameGenerator;
	float                       m_flRate;
};

