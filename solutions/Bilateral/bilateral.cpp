//
// OpenCL host<->device transfer exercse
//

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#include <SDL2/SDL.h>

#define __CL_ENABLE_EXCEPTIONS
#include <cl.hpp>
#include <device_picker.hpp>
#include <util.hpp>

#undef main
#undef min
#undef max

void parseArguments(int argc, char *argv[]);
void runReference(uint8_t *input, uint8_t *output, int width, int height);

// Parameters, with default values.
unsigned deviceIndex   =      0;
unsigned iterations    =     32;
unsigned tolerance     =      1;
float    sigmaDomain   =      3.f;
float    sigmaRange    =      0.2f;
cl::NDRange wgsize     = cl::NullRange;
const char *inputFile  =  "1080p.bmp";

int main(int argc, char *argv[])
{
  try
  {
    parseArguments(argc, argv);

    // Get list of devices
    std::vector<cl::Device> devices;
    getDeviceList(devices);

    // Check device index in range
    if (deviceIndex >= devices.size())
    {
      std::cout << "Invalid device index (try '--list')" << std::endl;
      return 1;
    }

    cl::Device device = devices[deviceIndex];

    std::string name = device.getInfo<CL_DEVICE_NAME>();
    std::cout << std::endl << "Using OpenCL device: " << name << std::endl
              << std::endl;

    cl::Context context(device);
    cl::CommandQueue queue(context);
    cl::Program program(context, util::loadProgram("bilateral.cl"), false);
    try
    {
      std::stringstream options;
      options.setf(std::ios::fixed, std::ios::floatfield);
      options << " -cl-fast-relaxed-math";
      options << " -cl-single-precision-constant";
      program.build(options.str().c_str());
    }
    catch (cl::Error error)
    {
      if (error.err() == CL_BUILD_PROGRAM_FAILURE)
      {
        std::string log = program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(device);
        std::cerr << log << std::endl;
      }
      throw(error);
    }
    cl::make_kernel<cl::Image2D, cl::Image2D, cl_float, cl_float>
      kernel(program, "bilateral");

    // Load input image
    SDL_Surface *image = SDL_LoadBMP(inputFile);
    if (!image)
    {
      std::cout << SDL_GetError() << std::endl;
      throw;
    }

    cl::ImageFormat format(CL_RGBA, CL_UNORM_INT8);
    cl::Image2D input(context, CL_MEM_READ_ONLY, format, image->w, image->h);
    cl::Image2D output(context, CL_MEM_WRITE_ONLY, format, image->w, image->h);

    // Write image to device
    cl::size_t<3> origin;
    origin[0] = 0;
    origin[1] = 0;
    origin[2] = 0;
    cl::size_t<3> region;
    region[0] = image->w;
    region[1] = image->h;
    region[2] = 1;
    queue.enqueueWriteImage(input, CL_TRUE, origin, region,
                            0, 0, image->pixels);


    cl::NDRange global(image->w, image->h);

    // Apply filter
    std::cout << "Running OpenCL..." << std::endl;
    util::Timer timer;
    uint64_t startTime = timer.getTimeMicroseconds();
    for (unsigned i = 0; i < iterations; i++)
    {
      kernel(cl::EnqueueArgs(queue, global, wgsize),
             input, output, sigmaDomain, sigmaRange);
    }
    queue.finish();
    uint64_t endTime = timer.getTimeMicroseconds();
    double total = ((endTime-startTime)*1e-3);
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "OpenCL took " << total << "ms"
              << " (" << (total/iterations) << "ms / frame)"
              << std::endl << std::endl;

    // Save result to file
    SDL_Surface *result = SDL_ConvertSurface(image,
                                             image->format, image->flags);
    SDL_LockSurface(result);
    queue.enqueueReadImage(output, CL_TRUE, origin, region,
                           0, 0, result->pixels);
    SDL_UnlockSurface(result);
    SDL_SaveBMP(result, "output.bmp");


    // Run reference
    std::cout << "Running reference..." << std::endl;
    uint8_t *reference = new uint8_t[image->w*image->h*4];
    SDL_LockSurface(image);
    startTime = timer.getTimeMicroseconds();
    runReference((uint8_t*)image->pixels, reference, image->w, image->h);
    endTime = timer.getTimeMicroseconds();
    std::cout << "Reference took " << ((endTime-startTime)*1e-3) << "ms"
              << std::endl << std::endl;

    // Check results
    unsigned errors = 0;
    for (int y = 0; y < result->h; y++)
    {
      for (int x = 0; x < result->w; x++)
      {
        for (int c = 0; c < 3; c++)
        {
          uint8_t out = ((uint8_t*)result->pixels)[(x + y*result->w)*4 + c];
          uint8_t ref = reference[(x + y*result->w)*4 + c];
          unsigned diff = abs((int)ref-(int)out);
          if (diff > tolerance)
          {
            if (!errors)
            {
              std::cout << "Verification failed:" << std::endl;
            }

            // Only show the first 8 errors
            if (errors++ < 8)
            {
              std::cout << "(" << x << "," << y << "," << c << "): "
                        << (int)out << " vs " << (int)ref << std::endl;
            }
          }
        }
      }
    }
    if (errors)
    {
      std::cout << "Total errors: " << errors << std::endl;
    }
    else
    {
      std::cout << "Verification passed." << std::endl;
    }
    SDL_UnlockSurface(result);

    delete[] reference;
  }
  catch (cl::Error err)
  {
    std::cout << "Exception:" << std::endl
              << "ERROR: "
              << err.what()
              << "("
              << err_code(err.err())
              << ")"
              << std::endl;
  }
  std::cout << std::endl;

#if defined(_WIN32) && !defined(__MINGW32__)
  system("pause");
#endif

  return 0;
}

int parseFloat(const char *str, cl_float *output)
{
  char *next;
  *output = (cl_float)strtod(str, &next);
  return !strlen(next);
}

void parseArguments(int argc, char *argv[])
{
  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--list"))
    {
      // Get list of devices
      std::vector<cl::Device> devices;
      getDeviceList(devices);

      // Print device names
      if (devices.size() == 0)
      {
        std::cout << "No devices found." << std::endl;
      }
      else
      {
        std::cout << std::endl;
        std::cout << "Devices:" << std::endl;
        for (unsigned i = 0; i < devices.size(); i++)
        {
          std::string name = devices[i].getInfo<CL_DEVICE_NAME>();
          std::cout << i << ": " << name << std::endl;
        }
        std::cout << std::endl;
      }
      exit(0);
    }
    else if (!strcmp(argv[i], "--device"))
    {
      if (++i >= argc || !parseUInt(argv[i], &deviceIndex))
      {
        std::cout << "Invalid device index" << std::endl;
        exit(1);
      }
    }
    else if (!strcmp(argv[i], "--image"))
    {
      if (++i >= argc)
      {
        std::cout << "Missing argument to --image" << std::endl;
        exit(1);
      }
      inputFile = argv[i];
    }
    else if (!strcmp(argv[i], "--iterations") || !strcmp(argv[i], "-i"))
    {
      if (++i >= argc || !parseUInt(argv[i], &iterations))
      {
        std::cout << "Invalid number of iterations" << std::endl;
        exit(1);
      }
    }
    else if (!strcmp(argv[i], "--sd"))
    {
      if (++i >= argc || !parseFloat(argv[i], &sigmaDomain))
      {
        std::cout << "Invalid sigma domain" << std::endl;
        exit(1);
      }
    }
    else if (!strcmp(argv[i], "--sr"))
    {
      if (++i >= argc || !parseFloat(argv[i], &sigmaRange))
      {
        std::cout << "Invalid sigma range" << std::endl;
        exit(1);
      }
    }
    else if (!strcmp(argv[i], "--wgsize"))
    {
      unsigned width, height;
      if (++i >= argc || !parseUInt(argv[i], &width))
      {
        std::cout << "Invalid work-group width" << std::endl;
        exit(1);
      }
      if (++i >= argc || !parseUInt(argv[i], &height))
      {
        std::cout << "Invalid work-group height" << std::endl;
        exit(1);
      }
      wgsize = cl::NDRange(width, height);
    }
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
    {
      std::cout << std::endl;
      std::cout << "Usage: ./bilateral [OPTIONS]" << std::endl << std::endl;
      std::cout << "Options:" << std::endl;
      std::cout << "  -h  --help               Print the message" << std::endl;
      std::cout << "      --list               List available devices" << std::endl;
      std::cout << "      --device     INDEX   Select device at INDEX" << std::endl;
      std::cout << "      --image      FILE    Use FILE as input (must be 32-bit RGBA)" << std::endl;
      std::cout << "  -i  --iterations ITRS    Number of benchmark iterations" << std::endl;
      std::cout << "      --sd         D       Set sigma domain" << std::endl;
      std::cout << "      --sr         R       Set sigma range" << std::endl;
      std::cout << "      --wgsize     W H     Work-group width and height" << std::endl;
      std::cout << std::endl;
      exit(0);
    }
    else
    {
      std::cout << "Unrecognized argument '" << argv[i] << "' (try '--help')"
                << std::endl;
      exit(1);
    }
  }
}

void runReference(uint8_t *input, uint8_t *output,
                  int width, int height)
{
  for (int y = 0; y < height; y++)
  {
    for (int x = 0; x < width; x++)
    {
      float cr = input[(x + y*width)*4 + 0]/255.f;
      float cg = input[(x + y*width)*4 + 1]/255.f;
      float cb = input[(x + y*width)*4 + 2]/255.f;

      float coeff = 0.f;
      float sr = 0.f;
      float sg = 0.f;
      float sb = 0.f;

      for (int j = -2; j <= 2; j++)
      {
        for (int i = -2; i <= 2; i++)
        {
          int _x = std::min(std::max(x+i, 0), width-1);
          int _y = std::min(std::max(y+j, 0), height-1);

          float r = input[(_x + _y*width)*4 + 0]/255.f;
          float g = input[(_x + _y*width)*4 + 1]/255.f;
          float b = input[(_x + _y*width)*4 + 2]/255.f;

          float weight, norm;

          norm = sqrt((float)(i*i) + (float)(j*j)) * (1.f/sigmaDomain);
          weight = exp(-0.5f * (norm*norm));

          norm = sqrt(pow(r-cr,2) + pow(g-cg,2) + pow(b-cb,2)) * (1.f/sigmaRange);
          weight *= exp(-0.5f * (norm*norm));

          coeff += weight;
          sr += weight * r;
          sg += weight * g;
          sb += weight * b;
        }
      }
      output[(x + y*width)*4 + 0] = (uint8_t)(std::min(std::max(sr/coeff, 0.f), 1.f)*255.f);
      output[(x + y*width)*4 + 1] = (uint8_t)(std::min(std::max(sg/coeff, 0.f), 1.f)*255.f);
      output[(x + y*width)*4 + 2] = (uint8_t)(std::min(std::max(sb/coeff, 0.f), 1.f)*255.f);
      output[(x + y*width)*4 + 3] = input[(x + y*width)*4 + 3];
    }
  }
}
