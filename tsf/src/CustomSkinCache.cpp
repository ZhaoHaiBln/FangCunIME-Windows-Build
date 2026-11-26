#include "CustomSkinCache.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <map>
#include <limits>
#include <utility>

namespace fangcun::tsf {
namespace {
using Microsoft::WRL::ComPtr;
struct Handle { HANDLE h=INVALID_HANDLE_VALUE; ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);} };
bool directory(const std::filesystem::path& path) {
    const auto attrs=GetFileAttributesW(path.c_str());
    return attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY)&&!(attrs&FILE_ATTRIBUTE_REPARSE_POINT);
}
std::optional<std::wstring> directoryPath(HANDLE directoryHandle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if(directoryHandle==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(directoryHandle,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return {};
    std::wstring result(32768,L'\0');
    const DWORD size=GetFinalPathNameByHandleW(directoryHandle,result.data(),static_cast<DWORD>(result.size()),FILE_NAME_NORMALIZED);
    if(!size||size>=result.size())return {};
    result.resize(size);return result;
}
bool isInside(HANDLE file, std::wstring_view expectedPrefix) {
    std::wstring actual(32768,L'\0');
    const DWORD size=GetFinalPathNameByHandleW(file,actual.data(),static_cast<DWORD>(actual.size()),FILE_NAME_NORMALIZED);
    return size>expectedPrefix.size()&&size<actual.size()&&CompareStringOrdinal(actual.data(),static_cast<int>(expectedPrefix.size()),expectedPrefix.data(),static_cast<int>(expectedPrefix.size()),TRUE)==CSTR_EQUAL;
}
std::optional<std::vector<uint8_t>> readFile(const std::filesystem::path& path,size_t limit,std::wstring_view expectedPrefix) {
    Handle file;file.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file.h==INVALID_HANDLE_VALUE||GetFileType(file.h)!=FILE_TYPE_DISK||!isInside(file.h,expectedPrefix))return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file.h,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))return {};
    LARGE_INTEGER size{};if(!GetFileSizeEx(file.h,&size)||size.QuadPart<=0||uint64_t(size.QuadPart)>limit)return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));size_t offset=0;
    while(offset<bytes.size()) {DWORD got=0;if(!ReadFile(file.h,bytes.data()+offset,static_cast<DWORD>(bytes.size()-offset),&got,nullptr)||!got)return {};offset+=got;}
    return bytes;
}
bool safeParents(const std::filesystem::path& packRoot,std::string_view relative) {
    auto parent=std::filesystem::path(std::string(relative)).parent_path();auto current=packRoot;
    for(const auto& part:parent) {current/=part;if(!directory(current))return false;}
    return true;
}
uint32_t be32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
std::shared_ptr<const SkinPixels> decode(IWICImagingFactory* factory,std::vector<uint8_t>& bytes,size_t remainingPixels) {
    static constexpr uint8_t signature[]{137,80,78,71,13,10,26,10};
    if(!factory||bytes.size()<33||!std::equal(std::begin(signature),std::end(signature),bytes.begin())||be32(bytes.data()+8)!=13||std::string_view(reinterpret_cast<const char*>(bytes.data()+12),4)!="IHDR")return {};
    const auto w=be32(bytes.data()+16),h=be32(bytes.data()+20);
    if(!w||!h||w>2048||h>2048||size_t(w)*h>remainingPixels||bytes[24]!=8||(bytes[25]!=2&&bytes[25]!=6)||bytes[26]!=0||bytes[27]!=0||bytes[28]!=0)return {};
    bool ended=false;
    for(size_t offset=8;offset+12<=bytes.size();) {
        const size_t length=be32(bytes.data()+offset);
        if(length>bytes.size()-offset-12)return {};
        const std::string_view type(reinterpret_cast<const char*>(bytes.data()+offset+4),4);
        if(type=="acTL"||type=="fcTL"||type=="fdAT")return {};
        offset+=length+12;
        if(type=="IEND"){if(length||offset!=bytes.size())return {};ended=true;break;}
    }
    if(!ended)return {};
    ComPtr<IWICStream> stream;if(FAILED(factory->CreateStream(stream.GetAddressOf()))||FAILED(stream->InitializeFromMemory(bytes.data(),static_cast<DWORD>(bytes.size()))))return {};
    ComPtr<IWICBitmapDecoder> decoder;if(FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,decoder.GetAddressOf())))return {};
    GUID format{};UINT count=0;if(FAILED(decoder->GetContainerFormat(&format))||format!=GUID_ContainerFormatPng||FAILED(decoder->GetFrameCount(&count))||count!=1)return {};
    ComPtr<IWICBitmapFrameDecode> frame;if(FAILED(decoder->GetFrame(0,frame.GetAddressOf())))return {};
    UINT fw=0,fh=0;if(FAILED(frame->GetSize(&fw,&fh))||fw!=w||fh!=h)return {};
    ComPtr<IWICFormatConverter> converter;if(FAILED(factory->CreateFormatConverter(converter.GetAddressOf()))||FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return {};
    auto out=std::make_shared<SkinPixels>();out->width=w;out->height=h;out->bgra.resize(size_t(w)*h*4);
    if(FAILED(converter->CopyPixels(nullptr,w*4,static_cast<UINT>(out->bgra.size()),out->bgra.data())))return {};
    return out;
}
std::shared_ptr<const LoadedCustomSkin> load(const std::filesystem::path& root,const std::string& id,const std::string& hash,bool dark,bool images,IWICImagingFactory* factory) {
    if(!skin::validId(id)||!skin::validHash(hash))return {};
    // Anchor the whole load to one opened application data directory. Resolving
    // packRoot for each file would trust a replaced skins junction as a new
    // root and let the file and comparison root drift outside together.
    Handle trustedRoot;
    trustedRoot.h=CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    const auto canonicalRoot=directoryPath(trustedRoot.h);if(!canonicalRoot)return {};
    std::wstring expectedPrefix=*canonicalRoot;
    if(expectedPrefix.back()!=L'\\')expectedPrefix+=L'\\';
    expectedPrefix+=L"skins\\";expectedPrefix.append(hash.begin(),hash.end());expectedPrefix+=L'\\';
    if(!directory(root/"skins"))return {};
    const auto packRoot=root/"skins"/hash;if(!directory(packRoot))return {};
    auto ini=readFile(packRoot/"resolved.ini",16384,expectedPrefix);if(!ini)return {};
    auto spec=skin::parseResolvedSkinIni(std::string_view(reinterpret_cast<const char*>(ini->data()),ini->size()),id,hash);if(!spec)return {};
    auto result=std::make_shared<LoadedCustomSkin>();result->spec=std::move(*spec);result->dark=dark;
    if(!images||!factory)return result;
    size_t pixels=0;std::map<std::string,std::shared_ptr<const SkinPixels>> decoded;
    for(size_t i=0;i<8;++i) {
        const auto& image=result->theme().images[i];if(!image)continue;
        if(auto cached=decoded.find(image->file);cached!=decoded.end()){result->pixels[i]=cached->second;continue;}
        std::shared_ptr<const SkinPixels> bitmap;
        if(safeParents(packRoot,image->file))if(auto bytes=readFile(packRoot/image->file,4194304,expectedPrefix))bitmap=decode(factory,*bytes,16777216-pixels);
        if(bitmap&&image->scale==skin::ImageScale::NineSlice&&(image->insets[0]+image->insets[2]>=bitmap->width||image->insets[1]+image->insets[3]>=bitmap->height))bitmap.reset();
        if(bitmap)pixels+=size_t(bitmap->width)*bitmap->height;
        decoded.emplace(image->file,bitmap);result->pixels[i]=std::move(bitmap);
    }
    return result;
}
std::shared_ptr<const LoadedBuiltinStyle> loadBuiltinStyle(const std::filesystem::path& root,const std::string& id,const std::string& hash,bool dark) {
    if(!skin::validBuiltinStyleId(id)||!skin::validHash(hash))return {};
    Handle trustedRoot;
    trustedRoot.h=CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    const auto canonicalRoot=directoryPath(trustedRoot.h);if(!canonicalRoot)return {};
    const auto stylesRoot=root/"builtin-styles",builtinRoot=stylesRoot/id,profileRoot=builtinRoot/hash;
    if(!directory(stylesRoot)||!directory(builtinRoot)||!directory(profileRoot))return {};
    std::wstring expectedPrefix=*canonicalRoot;
    if(expectedPrefix.back()!=L'\\')expectedPrefix+=L'\\';
    expectedPrefix+=L"builtin-styles\\";expectedPrefix.append(id.begin(),id.end());expectedPrefix+=L'\\';
    expectedPrefix.append(hash.begin(),hash.end());expectedPrefix+=L'\\';
    auto ini=readFile(profileRoot/"resolved.ini",16384,expectedPrefix);if(!ini)return {};
    auto spec=skin::parseBuiltinSkinStyleIni(std::string_view(reinterpret_cast<const char*>(ini->data()),ini->size()),id,hash);
    if(!spec)return {};
    auto result=std::make_shared<LoadedBuiltinStyle>();result->spec=std::move(*spec);result->dark=dark;return result;
}
}
CustomSkinCache::CustomSkinCache():worker_([this]{run();}){}
CustomSkinCache::~CustomSkinCache(){stop();}
void CustomSkinCache::stop(){ {std::lock_guard lock(mutex_);stopped_=true;++generation_;pending_.reset();ready_.reset();builtinReady_.reset();}cv_.notify_all();if(worker_.joinable())worker_.join(); }
void CustomSkinCache::request(HWND hwnd,const std::filesystem::path& root,std::string id,std::string hash,bool dark,bool images) {
    {std::lock_guard lock(mutex_);if(stopped_)return;ready_.reset();builtinReady_.reset();pending_=Request{hwnd,root,std::move(id),std::move(hash),dark,images,++generation_,false};}cv_.notify_one();
}
void CustomSkinCache::requestBuiltinStyle(HWND hwnd,const std::filesystem::path& root,std::string id,std::string hash,bool dark) {
    {std::lock_guard lock(mutex_);if(stopped_)return;ready_.reset();builtinReady_.reset();pending_=Request{hwnd,root,std::move(id),std::move(hash),dark,false,++generation_,true};}cv_.notify_one();
}
std::shared_ptr<const LoadedCustomSkin> CustomSkinCache::takeReady(){std::lock_guard lock(mutex_);return ready_;}
std::shared_ptr<const LoadedBuiltinStyle> CustomSkinCache::takeBuiltinReady(){std::lock_guard lock(mutex_);return builtinReady_;}
void CustomSkinCache::run(){
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);ComPtr<IWICImagingFactory> factory;
    if(SUCCEEDED(initialized))CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf()));
    while(true){Request request;
        {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return stopped_||pending_.has_value();});if(stopped_)break;request=std::move(*pending_);pending_.reset();}
        std::shared_ptr<const LoadedCustomSkin> loaded;
        std::shared_ptr<const LoadedBuiltinStyle> builtin;
        try {if(request.builtin)builtin=loadBuiltinStyle(request.root,request.id,request.hash,request.dark);else loaded=load(request.root,request.id,request.hash,request.dark,request.images,factory.Get());}catch(...){loaded.reset();builtin.reset();}
        {std::lock_guard lock(mutex_);if(stopped_)break;if(request.generation!=generation_)continue;ready_=std::move(loaded);builtinReady_=std::move(builtin);if(request.hwnd)PostMessageW(request.hwnd,ReadyMessage,0,0);}
    }
    factory.Reset();if(SUCCEEDED(initialized))CoUninitialize();
}
} // namespace fangcun::tsf
