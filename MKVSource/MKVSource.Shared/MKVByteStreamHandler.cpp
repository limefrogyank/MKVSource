#include "pch.h"
#include "MKVSource.h"
#include "MKVByteStreamHandler.h"
#include <wrl\module.h>



ActivatableClass(MKVByteStreamHandler);

//-------------------------------------------------------------------
// CMPEG1ByteStreamHandler  class
//-------------------------------------------------------------------
//-------------------------------------------------------------------
// Constructor
//-------------------------------------------------------------------

MKVByteStreamHandler::MKVByteStreamHandler()
{
}

//-------------------------------------------------------------------
// Destructor
//-------------------------------------------------------------------

MKVByteStreamHandler::~MKVByteStreamHandler()
{
}

//-------------------------------------------------------------------
// IMediaExtension methods
//-------------------------------------------------------------------

//-------------------------------------------------------------------
// SetProperties
// Sets the configuration of the media byte stream handler
//-------------------------------------------------------------------
IFACEMETHODIMP MKVByteStreamHandler::SetProperties(ABI::Windows::Foundation::Collections::IPropertySet *pConfiguration)
{
	return S_OK;
}

//-------------------------------------------------------------------
// IMFByteStreamHandler methods
//-------------------------------------------------------------------

//-------------------------------------------------------------------
// BeginCreateObject
// Starts creating the media source.
//-------------------------------------------------------------------

HRESULT MKVByteStreamHandler::BeginCreateObject(
	/* [in] */ IMFByteStream *pByteStream,
	/* [in] */ LPCWSTR pwszURL,
	/* [in] */ DWORD dwFlags,
	/* [in] */ IPropertyStore *pProps,
	/* [out] */ IUnknown **ppIUnknownCancelCookie,  // Can be nullptr
	/* [in] */ IMFAsyncCallback *pCallback,
	/* [in] */ IUnknown *punkState                  // Can be nullptr
	)
{
	HRESULT hr = S_OK;
	try
	{
		if (pByteStream == nullptr)
		{
			ThrowException(E_POINTER);
		}

		if (pCallback == nullptr)
		{
			ThrowException(E_POINTER);
		}

		if ((dwFlags & MF_RESOLUTION_MEDIASOURCE) == 0)
		{
			ThrowException(E_INVALIDARG);
		}

		ComPtr<IMFAsyncResult> spResult;
		ComPtr<MKVSource> spSource = MKVSource::CreateInstance();

		ComPtr<IUnknown> spSourceUnk;
		ThrowIfError(spSource.As(&spSourceUnk));
		ThrowIfError(MFCreateAsyncResult(spSourceUnk.Get(), pCallback, punkState, &spResult));

		// Start opening the source. This is an async operation.
		// When it completes, the source will invoke our callback
		// and then we will invoke the caller's callback.
		ComPtr<MKVByteStreamHandler> spThis = this;
		spSource->OpenAsync(pByteStream).then([this, spThis, spResult, spSource](concurrency::task<void>& openTask)
		{
			try
			{
				if (spResult == nullptr)
				{
					ThrowIfError(MF_E_UNEXPECTED);
				}

				openTask.get();
			}
			catch (Exception ^exc)
			{
				if (spResult != nullptr)
				{
					spResult->SetStatus(exc->HResult);
				}
			}

			if (spResult != nullptr)
			{
				MFInvokeCallback(spResult.Get());
			}
		});

		if (ppIUnknownCancelCookie)
		{
			*ppIUnknownCancelCookie = nullptr;
		}
	}
	catch (Exception ^exc)
	{
		hr = exc->HResult;
	}

	return hr;
}

//-------------------------------------------------------------------
// EndCreateObject
// Completes the BeginCreateObject operation.
//-------------------------------------------------------------------

HRESULT MKVByteStreamHandler::EndCreateObject(
	/* [in] */ IMFAsyncResult *pResult,
	/* [out] */ MF_OBJECT_TYPE *pObjectType,
	/* [out] */ IUnknown **ppObject)
{
	if (pResult == nullptr || pObjectType == nullptr || ppObject == nullptr)
	{
		return E_POINTER;
	}

	HRESULT hr = S_OK;

	*pObjectType = MF_OBJECT_INVALID;
	*ppObject = nullptr;

	hr = pResult->GetStatus();

	if (SUCCEEDED(hr))
	{
		ComPtr<IUnknown> punkSource;
		hr = pResult->GetObject(&punkSource);
		if (SUCCEEDED(hr))
		{
			*pObjectType = MF_OBJECT_MEDIASOURCE;
			*ppObject = punkSource.Detach();
		}
	}

	return hr;
}


HRESULT MKVByteStreamHandler::CancelObjectCreation(IUnknown *pIUnknownCancelCookie)
{
	return E_NOTIMPL;
}

HRESULT MKVByteStreamHandler::GetMaxNumberOfBytesRequiredForResolution(QWORD* pqwBytes)
{
	return E_NOTIMPL;
}

