#define ASTERIA_INTERPOLATION_PROBE_LIBRARY
#include "../Port/InterpolationProbe.cpp"
#include "CameraMotionReconstruction.h"
#include <array>
#include <functional>
#include <limits>

namespace motion_fixture
{
using asteria_camera_motion::Parameters;
using Json = nlohmann::json;
constexpr uint32_t W = 64, H = 40;
constexpr size_t Pixels = size_t(W) * H;
using Matrix = std::array<float, 16>;
using Expected = std::function<std::array<float, 3>(uint32_t, uint32_t)>;
static uint64_t Checks = 0;

void Require(bool value, const std::string &message)
{
    ++Checks;
    if (!value) throw std::runtime_error(message);
}

Matrix Identity() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }

Parameters Default()
{
    Parameters value;
    value.inverseCurrentViewProjection = Identity(); value.previousViewProjection = Identity();
    value.extentFlags = {W, H, 5, 0};
    return value;
}

std::array<float, 3> UVExpected(uint32_t x, uint32_t y, float previousU, float previousV)
{
    if (previousU < 0 || previousV < 0 || previousU >= 1 || previousV >= 1) return {0,0,0};
    return {previousU - (float(x) + 0.5f) / W, previousV - (float(y) + 0.5f) / H, 1};
}

Expected ViewportExpected(Parameters parameters, std::vector<float> currentDepth, std::vector<float> previousDepth)
{
    return [parameters, currentDepth = std::move(currentDepth), previousDepth = std::move(previousDepth)](uint32_t x, uint32_t y) {
        const auto invalid = [] { return std::array<float,3>{0,0,0}; };
        const auto validDepth = [&](double depth) { return std::isfinite(depth) && depth >= parameters.depthLimits[0] &&
            depth <= parameters.depthLimits[1] && std::abs(depth - parameters.depthLimits[2]) > parameters.depthLimits[3]; };
        const double px = double(x) + 0.5, py = double(y) + 0.5, depth = currentDepth[size_t(y) * W + x];
        if (!validDepth(depth)) return invalid();
        std::array<double,4> clip{2 * px / W - 1, 1 - 2 * py / H, depth, 1};
        if (parameters.viewportModes[0] == 1)
        {
            const auto &r = parameters.currentViewportRectangle;
            clip = {2 * (px-r[0]) / r[2] - 1, 2 * (py-r[1]) / r[3] - 1,
                    (depth-parameters.viewportDepthRanges[0]) / (parameters.viewportDepthRanges[1]-parameters.viewportDepthRanges[0]), 1};
            if (clip[0] < -1 || clip[0] > 1 || clip[1] < -1 || clip[1] > 1 || clip[2] < 0 || clip[2] > 1) return invalid();
        }
        std::array<double,4> world{}, projected{};
        for (size_t row=0; row<4; ++row) for (size_t column=0; column<4; ++column)
            world[row] += double(parameters.inverseCurrentViewProjection[row*4+column]) * clip[column];
        if (!std::isfinite(world[3]) || std::abs(world[3]) <= 1e-7) return invalid();
        const double divisor = world[3]; for (double &value : world) value /= divisor;
        for (size_t row=0; row<4; ++row) for (size_t column=0; column<4; ++column)
            projected[row] += double(parameters.previousViewProjection[row*4+column]) * world[column];
        if (!std::isfinite(projected[3]) || projected[3] <= 1e-7) return invalid();
        const double ndcX=projected[0]/projected[3], ndcY=projected[1]/projected[3], ndcZ=projected[2]/projected[3];
        double u=(ndcX+1)*0.5, v=(1-ndcY)*0.5, predictedDepth=ndcZ;
        if (parameters.viewportModes[1] == 1)
        {
            if (ndcX < -1 || ndcX > 1 || ndcY < -1 || ndcY > 1 || ndcZ < 0 || ndcZ > 1) return invalid();
            const auto &r=parameters.previousViewportRectangle;
            u=(r[0]+r[2]*(ndcX+1)*0.5)/W; v=(r[1]+r[3]*(ndcY+1)*0.5)/H;
            predictedDepth=parameters.viewportDepthRanges[2]+(parameters.viewportDepthRanges[3]-parameters.viewportDepthRanges[2])*ndcZ;
        }
        if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || u >= 1 || v < 0 || v >= 1 ||
            !std::isfinite(predictedDepth) || predictedDepth < parameters.depthLimits[0] || predictedDepth > parameters.depthLimits[1]) return invalid();
        if ((parameters.extentFlags[2] & 4u) != 0)
        {
            const double sampled=previousDepth[size_t(std::floor(v*H))*W+size_t(std::floor(u*W))];
            const double tolerance=std::max(double(parameters.previousDepthControls[0]),
                double(parameters.previousDepthControls[1])*std::max(std::abs(sampled),std::abs(predictedDepth)));
            if (!validDepth(sampled) || std::abs(sampled-predictedDepth) > tolerance) return invalid();
        }
        return std::array<float,3>{float(u-px/W),float(v-py/H),1};
    };
}

struct Fixture
{
    Gpu gpu;
    asteria_camera_motion::Pipeline pipeline;
    std::array<VkImageView, 4> views{};
    std::filesystem::path output;
    explicit Fixture(std::filesystem::path path) : output(std::move(path))
    {
        gpu.Initialize(); gpu.images.reserve(8); gpu.buffers.reserve(4);
        for (uint32_t index = 0; index < 4; ++index)
        {
            const VkFormat format = index == 2 ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R32_SFLOAT;
            VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(gpu.physical, format, &properties);
            Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0, "required storage image format");
            gpu.images.emplace_back(); auto &image = gpu.images.back();
            image.description = Description(index == 2 ? FFX_SURFACE_FORMAT_R32G32_FLOAT : FFX_SURFACE_FORMAT_R32_FLOAT, W, H);
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {W,H,1};
            info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            Check(vkCreateImage(gpu.device, &info, nullptr, &image.handle), "vkCreateImage(motion fixture)");
            VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(gpu.device, image.handle, &requirements);
            VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memory.allocationSize = requirements.size;
            memory.memoryTypeIndex = gpu.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            Check(vkAllocateMemory(gpu.device, &memory, nullptr, &image.memory), "vkAllocateMemory(motion fixture)");
            Check(vkBindImageMemory(gpu.device, image.handle, image.memory, 0), "vkBindImageMemory(motion fixture)");
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = image.handle; view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            Check(vkCreateImageView(gpu.device, &view, nullptr, &views[index]), "vkCreateImageView(motion fixture)");
        }
        gpu.buffers.emplace_back(); auto &staging = gpu.buffers.back();
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; buffer.size = Pixels * 2 * sizeof(float);
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateBuffer(gpu.device, &buffer, nullptr, &staging.handle), "vkCreateBuffer(sized motion staging)");
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(gpu.device, staging.handle, &requirements);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memory.allocationSize = requirements.size;
        memory.memoryTypeIndex = gpu.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(gpu.device, &memory, nullptr, &staging.memory), "vkAllocateMemory(motion staging)");
        Check(vkBindBufferMemory(gpu.device, staging.handle, staging.memory, 0), "vkBindBufferMemory(motion staging)");
        std::ifstream shader("build/ReconstructMotion.spv", std::ios::binary | std::ios::ate);
        if (!shader) throw std::runtime_error("compiled reconstruction shader unavailable");
        const size_t bytes = size_t(shader.tellg()); Require(bytes % 4 == 0 && bytes > 20, "SPIR-V byte alignment");
        std::vector<uint32_t> words(bytes / 4); shader.seekg(0); shader.read(reinterpret_cast<char *>(words.data()), bytes);
        Require(bool(shader), "SPIR-V read completed"); pipeline.Initialize(gpu.device, gpu.physical, words);
    }
    ~Fixture()
    {
        if (gpu.device) vkDeviceWaitIdle(gpu.device);
        for (auto view : views) if (view) vkDestroyImageView(gpu.device, view, nullptr);
    }
    std::vector<float> Readback(uint32_t imageIndex, uint32_t channels)
    {
        auto &image = gpu.images[imageIndex]; auto &staging = gpu.buffers[0];
        gpu.Begin(); gpu.Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {W,H,1};
        vkCmdCopyImageToBuffer(gpu.command, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle, 1, &copy);
        gpu.Barrier(image, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT; host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = staging.handle; host.size = Pixels * channels * sizeof(float);
        vkCmdPipelineBarrier(gpu.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        gpu.Submit();
        std::vector<float> values(Pixels * channels); void *mapped = nullptr;
        Check(vkMapMemory(gpu.device, staging.memory, 0, values.size() * sizeof(float), 0, &mapped), "vkMapMemory(motion readback)");
        std::memcpy(values.data(), mapped, values.size() * sizeof(float)); vkUnmapMemory(gpu.device, staging.memory);
        return values;
    }
    void Save(const std::filesystem::path &path, const std::vector<float> &values)
    {
        std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(float));
        Require(bool(file), "raw float readback saved");
    }
    Json Run(const std::string &name, Parameters parameters, std::vector<float> currentDepth, std::vector<float> previousDepth, Expected expected)
    {
        Require(currentDepth.size() == Pixels && previousDepth.size() == Pixels, "fixture input extents match");
        gpu.Upload(gpu.images[0], gpu.buffers[0], currentDepth.data(), currentDepth.size() * sizeof(float));
        gpu.Upload(gpu.images[1], gpu.buffers[0], previousDepth.data(), previousDepth.size() * sizeof(float));
        gpu.Begin(); for (auto &image : gpu.images) gpu.Barrier(image, VK_IMAGE_LAYOUT_GENERAL);
        const asteria_camera_motion::Views bindings{views[0],views[1],views[2],views[3]};
        auto lifetime = pipeline.Record(gpu.command, bindings, parameters);
        Require(bool(lifetime), "dispatch lifetime retained before queue submission");
        gpu.Submit(); lifetime.reset();
        const auto motion = Readback(2, 2), confidence = Readback(3, 1);
        size_t valid = 0, expectedValid = 0; double maxError = 0, maxMotionPixels = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                const size_t index = size_t(y) * W + x; const auto value = expected(x,y);
                Require(std::isfinite(motion[index*2]) && std::isfinite(motion[index*2+1]) && std::isfinite(confidence[index]), name + " finite GPU outputs");
                Require(confidence[index] == value[2], name + " GPU confidence matches independent analytic expectation");
                const double error = std::max(std::abs(double(motion[index*2]) - value[0]), std::abs(double(motion[index*2+1]) - value[1]));
                Require(error < 2e-5, name + " GPU motion matches independent analytic expectation");
                maxError = std::max(maxError,error); valid += confidence[index] > 0; expectedValid += value[2] > 0;
                maxMotionPixels = std::max(maxMotionPixels, std::hypot(double(motion[index*2])*W, double(motion[index*2+1])*H));
            }
        const auto directory = output / name; std::filesystem::create_directories(directory);
        Save(directory / "motion_rg32f.bin",motion); Save(directory / "confidence_r32f.bin",confidence);
        Save(directory / "current_depth_r32f.bin",currentDepth); Save(directory / "previous_depth_r32f.bin",previousDepth);
        Json result{{"name",name},{"passed",true},{"gpu_dispatches",1},{"pixels",Pixels},{"confidence_valid_pixels",valid},
                    {"expected_valid_pixels",expectedValid},{"max_uv_error",maxError},{"max_motion_pixels",maxMotionPixels},
                    {"motion_readback_bytes",motion.size()*sizeof(float)},{"confidence_readback_bytes",confidence.size()*sizeof(float)},
                    {"parameters",{{"inverse_current_view_projection",parameters.inverseCurrentViewProjection},{"previous_view_projection",parameters.previousViewProjection},
                                   {"extent_flags",parameters.extentFlags},{"depth_limits",parameters.depthLimits},{"previous_depth_controls",parameters.previousDepthControls},
                                   {"current_viewport_rectangle",parameters.currentViewportRectangle},{"previous_viewport_rectangle",parameters.previousViewportRectangle},
                                   {"viewport_depth_ranges",parameters.viewportDepthRanges},{"viewport_modes",parameters.viewportModes}}},
                    {"motion_readback",(directory/"motion_rg32f.bin").string()},{"confidence_readback",(directory/"confidence_r32f.bin").string()}};
        std::cout << name << ": valid=" << valid << " max_error=" << maxError << '\n';
        return result;
    }
};

int Run()
{
    const std::filesystem::path output="results"; std::filesystem::create_directories(output);
    Json report{{"author","Anokianx"},{"passed",false},{"scope","isolated_actual_gpu_static_scene_camera_reprojection"},
                {"game_started",false},{"engine_motion_vectors_available",false},{"independent_object_motion_reconstructed",false},
                {"native_fg_ready",false},{"motion_convention",asteria_camera_motion::MotionConvention},
                {"depth_convention",asteria_camera_motion::DepthConvention},{"matrix_convention",asteria_camera_motion::MatrixConvention},
                {"confidence_semantics",asteria_camera_motion::ConfidenceMeaning},{"motion_scale_for_pixel_units",{W,H}},
                {"parameter_bytes",sizeof(Parameters)},{"native_viewport_convention",asteria_camera_motion::NativeViewportConvention},
                {"original_native_matrices_preserved",true},{"viewport_profile_inferred",false},{"main_view_verified",false},
                {"width",W},{"height",H},{"motion_format","VK_FORMAT_R32G32_SFLOAT"},{"confidence_format","VK_FORMAT_R32_SFLOAT"},
                {"parameter_resource_lifetime","retained_until_vkQueueWaitIdle_after_dispatch"},{"cases",Json::array()}};
    try
    {
        Require(sizeof(void*)==4,"GPU motion fixture is x86"); Fixture fixture(output);
        report["device_name"]=fixture.gpu.properties.deviceName; report["device_id"]=fixture.gpu.properties.deviceID;
        report["vendor_id"]=fixture.gpu.properties.vendorID; report["process_bits"]=sizeof(void*)*8;
        const auto plane=[](float value){ return std::vector<float>(Pixels,value); };
        const auto zero=[](uint32_t,uint32_t){ return std::array<float,3>{0,0,0}; };
        const auto stationary=[](uint32_t,uint32_t){ return std::array<float,3>{0,0,1}; };
        auto run=[&](const std::string &name,Parameters p,const std::vector<float> &depth,const std::vector<float> &previous,Expected expected) {
            report["cases"].push_back(fixture.Run(name,p,depth,previous,std::move(expected)));
        };
        run("stationary",Default(),plane(0.5f),plane(0.5f),stationary);
        auto p=Default(); p.previousViewProjection[3]=0.125f;
        run("camera_translation_x_four_pixels",p,plane(0.5f),plane(0.5f),[](uint32_t x,uint32_t y){return UVExpected(x,y,(float(x)+0.5f)/W+0.0625f,(float(y)+0.5f)/H);});
        p=Default(); p.previousViewProjection[7]=0.2f;
        run("camera_translation_y_up_four_pixels",p,plane(0.5f),plane(0.5f),[](uint32_t x,uint32_t y){return UVExpected(x,y,(float(x)+0.5f)/W,(float(y)+0.5f)/H-0.1f);});
        p=Default(); p.previousViewProjection={0,-1,0,0, 1,0,0,0, 0,0,1,0, 0,0,0,1};
        run("camera_roll_quarter_turn",p,plane(0.5f),plane(0.5f),[](uint32_t x,uint32_t y){return UVExpected(x,y,(float(y)+0.5f)/H,1-(float(x)+0.5f)/W);});
        p=Default(); p.previousViewProjection={1.2f,0.1f,0.2f,0.04f, -0.15f,0.8f,-0.1f,-0.02f, 0,0,1,0, 0,0,0,1};
        run("asymmetric_row_major_y_and_uv_units",p,plane(0.4f),plane(0.4f),[](uint32_t x,uint32_t y){float u=(float(x)+0.5f)/W,v=(float(y)+0.5f)/H;return UVExpected(x,y,1.2f*u-0.1f*v+0.01f,0.15f*u+0.8f*v+0.055f);});
        p=Default(); p.extentFlags[2]=1; p.inverseCurrentViewProjection[0]=2; p.inverseCurrentViewProjection[3]=0.4f;
        p.inverseCurrentViewProjection[5]=0.5f; p.inverseCurrentViewProjection[7]=-0.2f;
        run("inverse_current_matrix_applied",p,plane(0.5f),plane(0.5f),[](uint32_t x,uint32_t y){float u=(float(x)+0.5f)/W,v=(float(y)+0.5f)/H;return UVExpected(x,y,2*u-0.3f,0.5f*v+0.35f);});
        for (float depth:{0.0f,0.05f,0.95f,0.999f})
        {
            p=Default(); const float a=10.0f/(0.1f-10.0f),b=1.0f/(0.1f-10.0f);
            p.inverseCurrentViewProjection={1/1.3f,0,0,0, 0,1/1.7f,0,0, 0,0,0,-1, 0,0,1/b,a/b};
            p.previousViewProjection={1.3f,0,0,1.3f*0.02f, 0,1.7f,0,0, 0,0,a,b, 0,0,-1,0};
            run("perspective_translation_depth_"+std::to_string(depth),p,plane(depth),plane(depth),[=](uint32_t x,uint32_t y){
                const float inverseDistance=(10.0f-9.9f*depth); const float shift=0.013f*inverseDistance;
                return UVExpected(x,y,(float(x)+0.5f)/W+shift,(float(y)+0.5f)/H);
            });
        }
        for (const auto &item:std::vector<std::pair<std::string,float>>{{"negative_depth",-0.1f},{"beyond_far_depth",1.1f},{"clear_sky_depth",1.0f},
                {"clear_depth_epsilon",1.0f-0.0000003f},{"nan_depth",std::numeric_limits<float>::quiet_NaN()},{"infinite_depth",std::numeric_limits<float>::infinity()}})
            run(item.first,Default(),plane(item.second),plane(0.5f),zero);
        p=Default(); p.extentFlags[2]|=2; run("history_reset",p,plane(0.5f),plane(0.5f),zero);
        p=Default(); p.extentFlags[2]&=~1u; run("camera_flag_invalid",p,plane(0.5f),plane(0.5f),zero);
        p=Default(); p.inverseCurrentViewProjection.fill(0); run("singular_current_camera",p,plane(0.5f),plane(0.5f),zero);
        p=Default(); p.previousViewProjection.fill(0); run("singular_previous_camera",p,plane(0.5f),plane(0.5f),zero);
        p=Default(); p.inverseCurrentViewProjection[3]=std::numeric_limits<float>::quiet_NaN(); run("nonfinite_camera",p,plane(0.5f),plane(0.5f),zero);
        p=Default(); p.previousViewProjection[15]=-1; run("behind_previous_camera",p,plane(0.5f),plane(0.5f),zero);
        p=Default(); p.inverseCurrentViewProjection[14]=1; p.inverseCurrentViewProjection[15]=-0.5f;
        run("zero_homogeneous_world_w",p,plane(0.5f),plane(0.5f),zero);
        p=Default(); p.previousViewProjection[3]=3; run("previous_uv_out_of_bounds",p,plane(0.5f),plane(0.5f),zero);
        p=Default(); p.previousViewProjection[11]=0.7f; run("previous_clip_depth_out_of_bounds",p,plane(0.5f),plane(0.5f),zero);
        run("previous_depth_occluded",Default(),plane(0.5f),plane(0.1f),zero);
        run("previous_depth_disoccluded",Default(),plane(0.5f),plane(0.8f),zero);
        run("previous_depth_clear",Default(),plane(0.5f),plane(1),zero);
        run("previous_depth_nonfinite",Default(),plane(0.5f),plane(std::numeric_limits<float>::quiet_NaN()),zero);
        p=Default(); p.extentFlags[2]=1;
        run("optional_previous_depth_disabled",p,plane(0.5f),plane(std::numeric_limits<float>::quiet_NaN()),stationary);
        p=Default(); p.previousDepthControls[0]=0.002f; p.previousDepthControls[1]=0;
        run("previous_depth_within_tolerance",p,plane(0.5f),plane(0.501f),stationary);
        auto current=plane(0.25f),previous=plane(1);
        for (uint32_t y=0;y<H;++y) for (uint32_t x=0;x<W;++x) { current[size_t(y)*W+x]=((x/5)%2)?0.75f:0.25f; if(x+4<W)previous[size_t(y)*W+x+4]=current[size_t(y)*W+x]; }
        p=Default(); p.previousViewProjection[3]=0.125f;
        run("previous_depth_sample_uses_reprojected_uv",p,current,previous,[](uint32_t x,uint32_t y){return UVExpected(x,y,(float(x)+0.5f)/W+0.0625f,(float(y)+0.5f)/H);});
        using asteria_camera_motion::NativeViewport;
        constexpr float cf=63.0f/128.0f;
        const NativeViewport flipped{{cf,float(H)+cf,float(W),-float(H)},{0,1},true};
        const NativeViewport positive{{0,0,float(W),float(H)},{0,1},true};
        Require(asteria_camera_motion::ValidNativeViewport(flipped),"explicit negative-height viewport contract accepted");
        auto unknown=flipped; unknown.known=false;
        Require(!asteria_camera_motion::ValidNativeViewport(unknown),"unknown viewport is not an accepted mapping");
        std::array<double,3> ndc{},roundtrip{};
        Require(asteria_camera_motion::NativePixelToNdc(flipped,{0.5,0.5,0.4},ndc),"native pixel-to-NDC succeeds");
        Require(std::abs(ndc[0]-(2*(0.5-double(cf))/W-1))<1e-12 &&
                std::abs(ndc[1]-(1-2*(0.5-double(cf))/H))<1e-12,"native affine retains explicit pixel origin bias and Y orientation");
        Require(asteria_camera_motion::NativeNdcToPixel(flipped,ndc,roundtrip),"native NDC-to-pixel succeeds");
        Require(std::abs(roundtrip[0]-0.5)<1e-12 && std::abs(roundtrip[1]-0.5)<1e-12 && std::abs(roundtrip[2]-0.4)<1e-12,
                "native affine CPU round trip retains pixel center");
        const auto nativeParameters=[&](NativeViewport a,NativeViewport b) {
            Parameters value=Default(); const auto originalCurrent=value.inverseCurrentViewProjection,originalPrevious=value.previousViewProjection;
            asteria_camera_motion::SetNativeViewports(value,a,b);
            Require(value.inverseCurrentViewProjection==originalCurrent && value.previousViewProjection==originalPrevious,"viewport mapping does not rewrite native matrices");
            return value;
        };
        const auto runNative=[&](const std::string &name,Parameters value,float currentValue=0.5f,float previousValue=0.5f) {
            auto a=plane(currentValue),b=plane(previousValue);
            auto expected=ViewportExpected(value,a,b);
            run(name,value,a,b,std::move(expected));
        };
        p=nativeParameters(flipped,flipped); runNative("native_negative_height_subpixel_stationary",p);
        Require(report["cases"].back()["max_motion_pixels"].get<double>()<0.0001,"matching native raw viewport has zero static motion");
        p.previousViewProjection[3]=0.0625f; runNative("native_negative_height_translation_x_two_pixels",p);
        p=nativeParameters(positive,positive); runNative("native_positive_height_stationary",p);
        p.previousViewProjection[7]=0.2f;
        run("native_positive_height_translation_y_down_four_pixels",p,plane(0.5f),plane(0.5f),[](uint32_t x,uint32_t y){
            return UVExpected(x,y,(float(x)+0.5f)/W,(float(y)+0.5f)/H+0.1f);});
        auto shifted=flipped; shifted.rectangle[0]+=0.75f; shifted.rectangle[1]-=0.25f;
        p=nativeParameters(flipped,shifted); runNative("native_previous_origin_subpixel_shift",p);
        p=nativeParameters(shifted,flipped); runNative("native_current_origin_subpixel_shift",p);
        const NativeViewport smaller{{16,30,32,-20},{0,1},true};
        p=nativeParameters(flipped,smaller); runNative("native_previous_extent_and_origin_change",p);
        const NativeViewport subrectangle{{8,32,48,-24},{0,1},true};
        p=nativeParameters(subrectangle,flipped); runNative("native_current_subrectangle_rejects_outside_pixels",p);
        p=nativeParameters(positive,flipped); runNative("native_current_positive_previous_negative_height",p);
        auto rangedCurrent=flipped,rangedPrevious=flipped;
        rangedCurrent.depthRange={0.2f,0.8f}; rangedPrevious.depthRange={0.1f,0.9f};
        p=nativeParameters(rangedCurrent,rangedPrevious); runNative("native_distinct_depth_ranges",p,0.38f,0.34f);
        rangedCurrent.depthRange={0.8f,0.2f}; rangedPrevious.depthRange={0.2f,0.8f};
        p=nativeParameters(rangedCurrent,rangedPrevious); runNative("native_reversed_current_depth_range",p,0.62f,0.38f);
        p=nativeParameters(flipped,flipped); p.previousViewProjection={1.1f,0.2f,0.05f,0.04f,-0.1f,0.9f,-0.04f,0.02f,0,0,1,0,0,0,0,1};
        runNative("native_affine_and_asymmetric_camera_composed",p);
        p=nativeParameters(flipped,flipped); p.viewportModes[0]=0; runNative("native_previous_with_default_current",p);
        p=nativeParameters(flipped,flipped); p.viewportModes[1]=0; runNative("native_current_with_default_previous",p);
        p=nativeParameters(flipped,flipped); p.extentFlags[2]|=2; run("native_history_reset",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(unknown,flipped); run("native_unknown_current_viewport",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(flipped,unknown); run("native_unknown_previous_viewport",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(flipped,flipped); p.currentViewportRectangle[2]=0; run("native_zero_current_width",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(flipped,flipped); p.previousViewportRectangle[3]=0; run("native_zero_previous_height",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(flipped,flipped); p.currentViewportRectangle[2]=-float(W); run("native_negative_current_width",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(flipped,flipped); p.viewportModes[0]=77; run("native_unknown_coordinate_mode",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(flipped,flipped); p.currentViewportRectangle[0]=std::numeric_limits<float>::quiet_NaN(); run("native_nonfinite_current_origin",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(flipped,flipped); p.previousViewportRectangle[3]=std::numeric_limits<float>::infinity(); run("native_nonfinite_previous_height",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(flipped,flipped); p.viewportDepthRanges[0]=-0.1f; run("native_depth_range_outside_zero_to_one",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(flipped,flipped); p.viewportDepthRanges[2]=p.viewportDepthRanges[3]=0.4f; run("native_degenerate_previous_depth_range",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(flipped,flipped); p.viewportDepthRanges[1]=std::numeric_limits<float>::quiet_NaN(); run("native_nonfinite_depth_range",p,plane(0.5f),plane(0.5f),zero);
        p=nativeParameters(rangedPrevious,rangedPrevious); run("native_depth_outside_current_viewport_range",p,plane(0.1f),plane(0.1f),zero);
        p=nativeParameters(flipped,flipped); run("native_previous_depth_mismatch",p,plane(0.5f),plane(0.1f),zero);
        report["gpu_dispatches"]=report["cases"].size(); report["checks"]=Checks; report["passed"]=true;
    }
    catch(const std::exception &error) { report["error"]=error.what(); report["checks"]=Checks; std::cerr<<error.what()<<'\n'; }
    std::ofstream file(output/"gpu_motion_contract.json"); file<<report.dump(2)<<'\n';
    std::cout<<"GPU camera motion passed="<<report["passed"]<<" dispatches="<<report["cases"].size()<<" checks="<<Checks<<'\n';
    return report["passed"].get<bool>()?0:1;
}
}

int main() { return motion_fixture::Run(); }
