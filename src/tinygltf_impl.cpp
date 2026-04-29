// Implementation TU for tinygltf.
//
// stb_image / stb_image_write must be enabled here so the TinyGLTF
// constructor's default LoadImageData / WriteImageData callbacks resolve at
// link time, even though M4a never invokes them. M4b will start using stb
// directly for texture upload — these symbols become first-class then.

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYGLTF_NO_EXTERNAL_IMAGE
#define TINYGLTF_USE_CPP14
#define JSON_NOEXCEPTION

#include <tiny_gltf.h>
