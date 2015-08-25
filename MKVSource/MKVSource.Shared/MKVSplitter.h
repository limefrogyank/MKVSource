#pragma once

namespace MKV
{
	public ref class MKVSplitter sealed
	{
	public:
		MKVSplitter();
		virtual ~MKVSplitter();
		void Initialize(Windows::Media::Core::MediaStreamSource ^ mss, Windows::Media::Core::VideoStreamDescriptor ^ videoDesc);
		void GenerateSample(Windows::Media::Core::MediaStreamSourceSampleRequest ^ request);

	private:
		Microsoft::WRL::ComPtr<IMFVideoSampleAllocator> _spSampleAllocator;
		//Microsoft::WRL::ComPtr<IMFVideoSampleAllocator> _spSampleAllocator;
		Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> _spDeviceManager;

	};
}
