#include <chrono>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>


#ifdef APP_EXE_SIGNATURE
#define LOADER_MSG_SIGNATURE            APP_EXE_SIGNATURE
#else
#define LOADER_MSG_SIGNATURE            0x4788CAFE
#endif

#ifdef APP_CHUNK_SIZE                   
#define LOADER_MSG_CHUNK_SIZE           APP_CHUNK_SIZE
#else
#define LOADER_MSG_CHUNK_SIZE           256
#endif

#ifdef APP_CHUNK_CRC_INITIAL            
#define LOADER_MSG_CHUNK_CRC_INITIAL    APP_CHUNK_CRC_INITIAL
#else
#define LOADER_MSG_CHUNK_CRC_INITIAL    0xffff
#endif

#define LOADER_UART_BAUDRATE            B115200

#ifndef LOADER_MAX_RETRY_AMOUNT
#define LOADER_MAX_RETRY_AMOUNT         3
#endif

#ifndef LOADER_RESPONSE_TIMEOUT_S       
#define LOADER_RESPONSE_TIMEOUT_S       5
#endif

#define LOADER_KIBI     1024
#define LOADER_MEBI     1024 * LOADER_KIBI


enum class loaderStatusCode
{
    LOADER_OK,

    LOADER_ERROR_ARGS,
    LOADER_ERROR_FILE_ACCESS,
    LOADER_ERROR_FILE_OP_WRITE,
    LOADER_ERROR_IMAGE_INVALID,

    LOADER_ERROR_MSG_SIGN,
    LOADER_ERROR_MSG_TIMEOUT,
    LOADER_ERROR_MSG_LENGTH_IMAGE,
    LOADER_ERROR_MSG_LENGTH_CHUNK,
    LOADER_ERROR_MSG_CHECKSUM_CHUNK,
    LOADER_ERROR_MSG_CHECKSUM_IMAGE,
    LOADER_ERROR_MSG_CHUNK_OFFSET,
    LOADER_ERROR_PAGE_VERIFY,

    LOADER_ERROR_MSG_RESPONSE_LENGTH,
    
    LOADER_ERROR_UNKNOWN,
};

enum class loaderResponseStatusCode : uint8_t
{
    LOADER_MSG_OK,

    LOADER_ERROR_MSG_SIGN,
    LOADER_ERROR_MSG_TIMEOUT,
    LOADER_ERROR_MSG_LENGTH_IMAGE,
    LOADER_ERROR_MSG_LENGTH_CHUNK,
    LOADER_ERROR_MSG_CHECKSUM_CHUNK,
    LOADER_ERROR_MSG_CHECKSUM_IMAGE,
    LOADER_ERROR_MSG_PAGE_VERIFY,
    
    LOADER_ERROR_MSG_UNKNOWN,
};

struct loaderPrologue
{
    uint32_t signature;
    uint32_t length;
    uint32_t checksum;
};

struct loaderMsgHeader
{
    uint32_t offset;
    uint16_t length;
    uint16_t crc;
};

struct loaderMsg
{
    loaderMsgHeader header;
    std::vector<uint8_t> data;
    
    loaderMsg() : data(LOADER_MSG_CHUNK_SIZE, 0)
    {
    }
};

struct loaderStatusTuple
{
    loaderStatusCode code;
    std::string desc;
};


struct loaderResponse
{
    loaderResponseStatusCode code;
    uint16_t crc;
};


/**
 * @brief 
 */
class ImageLoader
{
    public:
        ImageLoader(const std::string& fileImagePath, const std::string& fileDevicePath);
        ~ImageLoader();
        loaderStatusTuple imageProcess();
    
    private:
        std::string m_file_image;
        std::string m_file_device;
        int m_file_device_desc;

        inline loaderStatusTuple response2status(loaderResponseStatusCode responseCode);
        uint16_t calcCrc16(const uint8_t* pData, size_t length, const uint16_t initial = LOADER_MSG_CHUNK_CRC_INITIAL);
        int uartOpen(const char* pDevice);
        loaderStatusTuple dataSend(const uint8_t* pData, size_t length, 
                                        const unsigned retriesMax = LOADER_MAX_RETRY_AMOUNT,
                                        const unsigned timeOutS = LOADER_RESPONSE_TIMEOUT_S);
        loaderStatusTuple imageCheck(std::ifstream& fileImage);
};


/**
 * @brief 
 * @param fileImagePath 
 * @param fileDevicePath 
 */
ImageLoader::ImageLoader(const std::string& fileImagePath, const std::string& fileDevicePath)
    : m_file_image(fileImagePath), m_file_device(fileDevicePath), m_file_device_desc(-1)
{

} 

/**
 * @brief 
 */
ImageLoader::~ImageLoader()
{
    if(m_file_device_desc != -1)
    {
        close(m_file_device_desc);
    }
}


inline loaderStatusTuple ImageLoader::response2status(loaderResponseStatusCode responseCode)
{
    switch(responseCode)
    {
        case loaderResponseStatusCode::LOADER_MSG_OK:
            return {loaderStatusCode::LOADER_OK, "Data packet has been delivered successfully"};
        case loaderResponseStatusCode::LOADER_ERROR_MSG_SIGN:
            return {loaderStatusCode::LOADER_ERROR_MSG_SIGN, "Incorrect image signature"};
        case loaderResponseStatusCode::LOADER_ERROR_MSG_LENGTH_IMAGE:
            return {loaderStatusCode::LOADER_ERROR_MSG_LENGTH_IMAGE, "Incorrect image size"};
        case loaderResponseStatusCode::LOADER_ERROR_MSG_LENGTH_CHUNK:
            return {loaderStatusCode::LOADER_ERROR_MSG_LENGTH_CHUNK, "Incorrect chunk size"};
        case loaderResponseStatusCode::LOADER_ERROR_MSG_CHECKSUM_CHUNK:
            return {loaderStatusCode::LOADER_ERROR_MSG_CHECKSUM_CHUNK, "Incorrect data packet checksum"};
        case loaderResponseStatusCode::LOADER_ERROR_MSG_CHECKSUM_IMAGE:
            return {loaderStatusCode::LOADER_ERROR_MSG_CHECKSUM_IMAGE, "Incorrect image checksum"};
        case loaderResponseStatusCode::LOADER_ERROR_MSG_PAGE_VERIFY:
            return {loaderStatusCode::LOADER_ERROR_PAGE_VERIFY, "SPI flash page verification has failed"};
        case loaderResponseStatusCode::LOADER_ERROR_MSG_UNKNOWN:
        default:
            return {loaderStatusCode::LOADER_ERROR_UNKNOWN, "Error during UART device file select"};
    }
}

/**
 * @brief 
 * @param pData 
 * @param length 
 * @return 
 */
uint16_t ImageLoader::calcCrc16(const uint8_t* pData, size_t length, const uint16_t initial) 
{
    uint16_t crc = initial; // Initial value
    for(size_t i = 0; i < length; i++)
    {
        crc ^= pData[i];
        for(int j = 0; j < 8; ++j)
        {
            if(crc & 0x0001)
            {
                crc = (crc >> 1) ^ 0xA001;
            } 
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc;
}


int ImageLoader::uartOpen(const char* pDevice)
{
    int status = -1;
    int fileDesc = open(pDevice, O_RDWR | O_NOCTTY | O_NDELAY);
    if(fileDesc != -1)
    {
        struct termios options;

        status = tcgetattr(fileDesc, &options);
        if(status == 0)
        {
            /* Set the serial device baud rate */
            options.c_ispeed = LOADER_UART_BAUDRATE;
            options.c_ospeed = LOADER_UART_BAUDRATE;
            /* Enable receiver, ignore modem control lines */
            options.c_cflag |= (CLOCAL | CREAD);
            /* 8 data bits, no parity, 1 stop bit */
            options.c_cflag |= CS8;
            options.c_cflag &= ~PARENB;
            options.c_cflag &= ~CSTOPB;
            options.c_cflag &= ~CSIZE; 
            status = tcsetattr(fileDesc, TCSANOW, &options);
        }
    }

    return status == 0 ? fileDesc : -1;
}

loaderStatusTuple ImageLoader::dataSend(const uint8_t* pData, size_t length, 
                                            const unsigned retriesMax, const unsigned timeOutS)
{
    int deviceDesc = m_file_device_desc;
    int status;
    
    for(unsigned attempt = 0; attempt <= retriesMax; attempt++)
    {
        ssize_t bytesWritten = 0;
        ssize_t bytesRead = 0;
        uint16_t crc;
        
        bytesWritten = write(deviceDesc, pData, length);
        if(bytesWritten != static_cast<ssize_t>(length))
        {
            std::cerr<<"Error writing to UART device file, errno: "<<strerror(errno)<<std::endl;
            return {loaderStatusCode::LOADER_ERROR_FILE_OP_WRITE, "Failed to write all the bytes to UART device"};
        }
        
        // Wait for response with timeout
        fd_set read_fds;
        struct timeval timeout;
        
        FD_ZERO(&read_fds);
        FD_SET(deviceDesc, &read_fds);
        
        timeout.tv_sec = timeOutS; // Timeout of 5 seconds
        timeout.tv_usec = 0;

        status = select(deviceDesc + 1, &read_fds, nullptr, nullptr, &timeout);
        if(status == -1)
        {
            return {loaderStatusCode::LOADER_ERROR_FILE_ACCESS, "Error during UART device file select"};
        }
        else if(status == 0)
        {
            continue; // Timeout occurred, retry sending
        }

        struct loaderResponse response;
        loaderStatusTuple responseStatus;
        bytesRead = read(deviceDesc, &response, sizeof(response));
        if(bytesRead != sizeof(response))
        {
            return {loaderStatusCode::LOADER_ERROR_MSG_RESPONSE_LENGTH, "Response of invalid size has been received"};
        }
        crc = this->calcCrc16(reinterpret_cast<const uint8_t*>(&response.code), sizeof(loaderResponseStatusCode));
        if(crc != response.crc)
        {
            return {loaderStatusCode::LOADER_ERROR_MSG_CHECKSUM_CHUNK, "The response with incorrect checksum has been received"};
        }
        responseStatus = this->response2status(response.code);
        
        return responseStatus;
    }
    
    return {loaderStatusCode::LOADER_ERROR_MSG_TIMEOUT, "Max retries reached without success"};
}

loaderStatusTuple ImageLoader::imageCheck(std::ifstream& fileImage)
{
    size_t imageSize = 0;
    const uint32_t imageSignature = LOADER_MSG_SIGNATURE;
    loaderStatusTuple status;
    size_t bytesCounter = 0;
    loaderPrologue prologue;
    size_t imageHeaderSize = sizeof(prologue);

    if(!fileImage.is_open())
    {
        return {loaderStatusCode::LOADER_ERROR_FILE_ACCESS, "Cannot access the provided binary file"};
    }
    
    fileImage.seekg(0, std::ios::end);
    imageSize = fileImage.tellg();
    fileImage.seekg(0, std::ios::beg);

    /* Set up the specific first packet */
    fileImage.read(reinterpret_cast<char*>(&prologue), sizeof(prologue));
    bytesCounter += fileImage.gcount();
    if(bytesCounter != sizeof(prologue))
    {
        return {loaderStatusCode::LOADER_ERROR_FILE_ACCESS, "Failed to read image header"};
    }
    
    std::cout<<"Image magic word: "<<prologue.signature<<std::endl;
    std::cout<<"Image length: "<<prologue.length<<std::endl;
    std::cout<<"Image checksum: "<<prologue.checksum<<std::endl;

    if(prologue.signature != imageSignature)
    {
        return {loaderStatusCode::LOADER_ERROR_IMAGE_INVALID, "Incorrect image has been specified"};
    }
    if(prologue.length < LOADER_KIBI || prologue.length >= 4 * LOADER_MEBI || prologue.length != (imageSize - imageHeaderSize))
    {
        return {loaderStatusCode::LOADER_ERROR_IMAGE_INVALID, "Incorrect image has been specified"};
    }

    return {loaderStatusCode::LOADER_OK, "Image is valid"};
}

loaderStatusTuple ImageLoader::imageProcess()
{
    size_t imageSize = 0;
    size_t bytesCounter = 0;
    size_t bytesRead = 0;
    uint32_t imageOffset = 0;
    loaderMsg packet;
    loaderStatusTuple status;

    /* Open and setup the serial UART device */
    int fileDeviceDesc = ImageLoader::uartOpen(m_file_device.c_str());
    if(fileDeviceDesc < 0)
    {
        return {loaderStatusCode::LOADER_ERROR_FILE_ACCESS, "Cannot access or setup the provided serial device"};
    }
    else
    {
        m_file_device_desc = fileDeviceDesc;
    }

    /* Open the image file */
    std::ifstream fileImage(m_file_image, std::ios::binary);
    status = this->imageCheck(fileImage);
    if(status.code != loaderStatusCode::LOADER_OK)
    {
        return status;
    }
    fileImage.seekg(0, std::ios::beg);
    
    /* Continue data transferring */
    while(fileImage.read(reinterpret_cast<char*>(packet.data.data()), packet.data.size()))
    {
        bytesRead = fileImage.gcount();
        bytesCounter += bytesRead;
        
        packet.header.offset = imageOffset;
        packet.header.length = bytesRead;
        packet.header.crc = this->calcCrc16(static_cast<const uint8_t*>(packet.data.data()), packet.data.size());

        status = this->dataSend(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
        if(status.code != loaderStatusCode::LOADER_OK)
        {
            return status;
        }
        
        imageOffset += bytesRead;
        imageSize += bytesRead;
    }

    fileImage.close();
    close(m_file_device_desc);

    return {loaderStatusCode::LOADER_OK, "Image file has been transferred successfully"};
}


int main(int argc, char* argv[])
{
    if(argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <path_to_image> <path_to_uart_device>" << std::endl;
        return static_cast<int>(loaderStatusCode::LOADER_ERROR_ARGS);
    }

    const std::string fileImagePath(argv[1]);
    const std::string fileDevicePath(argv[2]);
    ImageLoader loader(fileImagePath, fileDevicePath);
    
    loaderStatusTuple status = loader.imageProcess();
    if(status.code != loaderStatusCode::LOADER_OK)
    {
        std::cerr << "Error: " << status.desc << std::endl;
    }
    else
    {
        std::cout << status.desc << std::endl;
    }

    return static_cast<int>(status.code);
}