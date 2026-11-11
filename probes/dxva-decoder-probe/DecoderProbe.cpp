// L15 probe (no system change): create the D3D11 H.264 VLD video decoder the way msmpeg2vdec does for
// the Camera app's recorded-clip preview (1280x720 NV12), on the hardware device. Each step is written
// and flushed before it runs, so a crash leaves the last step in the output. Usage:
//   DecoderProbe.exe <out.txt> [width height]
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#pragma comment(lib, "d3d11.lib")
static FILE *o;
static void L(const char *f, ...) { va_list a; va_start(a, f); vfprintf(o, f, a); va_end(a); fputc('\n', o); fflush(o); }
static const GUID H264_VLD_NOFGT = {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
int main(int argc, char **argv)
{
	UINT w = 1280, h = 720, i, n = 0, cfgs = 0; BOOL sup = FALSE; HRESULT hr;
	ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; ID3D11VideoDevice *vd = NULL; ID3D11VideoDecoder *dec = NULL;
	D3D_FEATURE_LEVEL fl; D3D11_VIDEO_DECODER_DESC desc; D3D11_VIDEO_DECODER_CONFIG cfg; GUID g;
	if (argc < 2 || fopen_s(&o, argv[1], "w")) return 2;
	if (argc >= 4) { w = atoi(argv[2]); h = atoi(argv[3]); }
	L("step create-device");
	hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
	L("create-device hr=0x%08X fl=0x%X", hr, fl); if (FAILED(hr)) return 3;
	hr = dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void **) &vd);
	L("video-device hr=0x%08X", hr); if (FAILED(hr)) return 4;
	n = vd->GetVideoDecoderProfileCount(); L("profiles=%u", n);
	for (i = 0; i < n; ++i) if (SUCCEEDED(vd->GetVideoDecoderProfile(i, &g)))
		L("profile %u {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", i, g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
	hr = vd->CheckVideoDecoderFormat(&H264_VLD_NOFGT, DXGI_FORMAT_NV12, &sup); L("check-format hr=0x%08X supported=%d", hr, sup);
	ZeroMemory(&desc, sizeof(desc)); desc.Guid = H264_VLD_NOFGT; desc.SampleWidth = w; desc.SampleHeight = h; desc.OutputFormat = DXGI_FORMAT_NV12;
	hr = vd->GetVideoDecoderConfigCount(&desc, &cfgs); L("config-count hr=0x%08X n=%u", hr, cfgs);
	for (i = 0; i < cfgs; ++i) {
		if (FAILED(vd->GetVideoDecoderConfig(&desc, i, &cfg))) continue;
		L("config %u bitstreamRaw=%u resid=%u", i, cfg.ConfigBitstreamRaw, cfg.ConfigResidDiffAccelerator);
		L("step create-decoder %u %ux%u", i, w, h);
		hr = vd->CreateVideoDecoder(&desc, &cfg, &dec);
		L("create-decoder %u hr=0x%08X", i, hr);
		if (dec) { dec->Release(); dec = NULL; }
	}
	L("done"); return 0;
}
