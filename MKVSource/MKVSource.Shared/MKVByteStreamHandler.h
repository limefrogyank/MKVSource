#pragma once


class MKVByteStreamHandler WrlSealed
	: public Microsoft::WRL::RuntimeClass <
	Microsoft::WRL::RuntimeClassFlags< Microsoft::WRL::RuntimeClassType::WinRtClassicComMix >,
	ABI::Windows::Media::IMediaExtension,
	IMFByteStreamHandler
	>
{
	InspectableClass(L"MKVSource.MKVByteStreamHandler", BaseTrust)

public:
	MKVByteStreamHandler();
	virtual ~MKVByteStreamHandler();

	// IMediaExtension
	IFACEMETHOD(SetProperties) (ABI::Windows::Foundation::Collections::IPropertySet *pConfiguration);

	// IMFAsyncCallback
	STDMETHODIMP GetParameters(DWORD *pdwFlags, DWORD *pdwQueue)
	{
		// Implementation of this method is optional.
		return E_NOTIMPL;
	}

	// IMFByteStreamHandler
	STDMETHODIMP BeginCreateObject(
		/* [in] */ IMFByteStream *pByteStream,
		/* [in] */ LPCWSTR pwszURL,
		/* [in] */ DWORD dwFlags,
		/* [in] */ IPropertyStore *pProps,
		/* [out] */ IUnknown **ppIUnknownCancelCookie,
		/* [in] */ IMFAsyncCallback *pCallback,
		/* [in] */ IUnknown *punkState);

	STDMETHODIMP EndCreateObject(
		/* [in] */ IMFAsyncResult *pResult,
		/* [out] */ MF_OBJECT_TYPE *pObjectType,
		/* [out] */ IUnknown **ppObject);

	STDMETHODIMP CancelObjectCreation(IUnknown *pIUnknownCancelCookie);
	STDMETHODIMP GetMaxNumberOfBytesRequiredForResolution(QWORD *pqwBytes);

};