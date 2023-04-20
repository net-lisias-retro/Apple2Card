/* ttftp.c - tiny tiny ftp. Seriously. Tiny.

  Copyright (c) 2023 Thorsten C. Brehm

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "ttftp.h"
#include "config.h"
#include "Apple2Arduino.h"
#include <Ethernet.h>

#ifdef USE_FTP

//#define FTP_DEBUG 1

#ifdef FTP_DEBUG
  #define FTP_DEBUG_PRINTLN(x) Serial.println(x)
#else
  #define FTP_DEBUG_PRINTLN(x) {}
#endif

/* size of the FTP data buffer on the stack (values >= 128 should work) */
#define FTP_BUF_SIZE          512
  
/* Timeout in milliseconds until clients have to connect to a passive connection */
#define FTP_PASV_TIMEOUT     3000

/* Timeout after which stale data connections are terminated. */
#define FTP_TRANSMIT_TIMEOUT 5000

#ifdef USE_RAW_BLOCKDEV
  // when raw block device mode is enabled, don't allow writing to BLOCKDEV0.
  #define FTP_FIRST_BLKDEV_FILE 1
#else
  #define FTP_FIRST_BLKDEV_FILE 0
#endif

/* Ethernet and MAC address ********************************************************************************/
// MAC+IP+Port address all packed into one compact data structure, to simplify configuration
byte FtpMacIpPortData[] = { FTP_MAC_ADDRESS, FTP_IP_ADDRESS, FTP_DATA_PORT>>8, FTP_DATA_PORT&0xff };
#define OFFSET_MAC 0
#define OFFSET_IP  6

/* Ethernet servers and clients ****************************************************************************/
EthernetServer FtpCmdServer(FTP_CMD_PORT);
EthernetServer FtpDataServer(FTP_DATA_PORT);
EthernetClient FtpCmdClient;
EthernetClient FtpDataClient;

/* Supported FTP commands **********************************************************************************/
/* FTP Command IDs*/
#define FTP_CMD_USER  0
#define FTP_CMD_SYST  1
#define FTP_CMD_CWD   2
#define FTP_CMD_TYPE  3
#define FTP_CMD_QUIT  4
#define FTP_CMD_PASV  5
#define FTP_CMD_LIST  6
#define FTP_CMD_CDUP  7
#define FTP_CMD_RETR  8
#define FTP_CMD_STOR  9
#define FTP_CMD_PWD  10

/* Matching FTP Command strings (4byte per command) */
const char FtpCommandList[] PROGMEM = "USER" "SYST" "CWD " "TYPE" "QUIT" "PASV" "LIST" "CDUP" "RETR" "STOR" "PWD\x00";
                                       //"RNFR" "RNTO" "EPSV SIZE DELE MKD RMD"

/* Data types *********************************************************************************************/
typedef enum {DIR_ROOT=0, DIR_SDCARD1=1, DIR_SDCARD2=2} TFtpDirectory;
typedef enum {FTP_DISABLED=-1, FTP_NOT_INITIALIZED=0, FTP_INITIALIZED=1, FTP_CONNECTED=2} TFtpState;

//                                          "-rw------- 1 volume: 123456789abcdef 12345678 Jun 10 1977 BLKDEV01.PO\r\n"
const char FILE_TEMPLATE[]        PROGMEM = "-rw------- 1 volume: ---             12345678 Jun 10 1977 BLKDEV01.PO\r\n";
#define FILE_TEMPLATE_LENGTH      (sizeof(FILE_TEMPLATE)-1)

const char DIR_TEMPLATE[]         PROGMEM = "drwx------ 1 DanII AppleII 1 Jun 10 1977 SD1\r\n";
#define DIR_TEMPLATE_LENGTH       (sizeof(DIR_TEMPLATE)-1)

const char ROOT_DIR_TEMPLATE[]    PROGMEM = "257 \"/SD1\"";
#define ROOT_DIR_TEMPLATE_LENGTH  (sizeof(ROOT_DIR_TEMPLATE)-1)

const char PASSIVE_MODE_REPLY[]   PROGMEM = "227 Entering Passive Mode (";

const char FTP_WELCOME[]          PROGMEM = "Welcome to DAN][, v" FW_VERSION ". Apple ][ Forever!";
const char FTP_NO_FILE[]          PROGMEM = "No such file/folder";
const char FTP_TOO_LARGE[]        PROGMEM = "File too large";
const char FTP_BAD_FILENAME[]     PROGMEM = "Bad filename";
const char FTP_DAN2[]             PROGMEM = "DAN][";
const char FTP_BYE[]              PROGMEM = "Bye.";
const char FTP_OK[]               PROGMEM = "Ok";
const char FTP_FAILED[]           PROGMEM = "Failed";

/* Local variables ****************************************************************************************/
// structure with local FTP variables
struct
{
  int8_t  State       = FTP_NOT_INITIALIZED;
  uint8_t CmdBytes    = 0;        // FTP command: current number of received command bytes
  uint8_t ParamBytes  = 0;        // FTP command: current number of received parameter bytes
  uint8_t CmdId       = 0;        // current FTP command
  uint8_t Directory   = DIR_ROOT; // current working directory
  uint8_t _file0, _file1; // remembered original configuration
  char    CmdData[12]; // must just be large enough to hold file names (11 bytes for "BLKDEVXX.PO")
} Ftp;

extern int __heap_start, *__brkval;
  
#if 1
  void FTP_CMD_REPLY(char* buf, uint16_t sz) {buf[sz] = '\r';buf[sz+1]='\n';FtpCmdClient.write(buf, sz+2);}
#else
  void FTP_CMD_REPLY(char* buf, uint16_t sz)  {buf[sz] = '\r';buf[sz+1]='\n';FtpCmdClient.write(buf, sz+2);Serial.write(buf, sz+2);}
#endif

// simple string compare - avoiding more expensive compare in stdlib
bool strMatch(const char* str1, const char* str2)
{
  do
  {
    if (*str1 != *(str2++))
      return false;
  } while (*(str1++)!=0);
  return true;
}

// copy a string from PROGMEM to a RAM buffer
uint8_t strReadProgMem(char* buf, const char* pProgMem)
{
  uint8_t sz=0;
  do
  {
    *buf = pgm_read_byte_near(pProgMem++);
    sz++;
  } while (*(buf++));
  return sz-1;
}

// map FTP command string to ID
int8_t ftpGetCmdId(char* buf, const char* command)
{
  uint32_t cmd = *((uint32_t*) command);
  int8_t CmdId = 0;
  strReadProgMem(buf, FtpCommandList);
  while (buf[CmdId] != 0)
  {
    if (cmd == *((uint32_t*) &buf[CmdId]))
      return (CmdId >> 2);
    CmdId += 4;
  }
  return -1;
}

char* strPrintInt(char* pStr, uint32_t data, uint32_t maxDigit=10000, char fillByte=0)
{
  uint32_t d = maxDigit;
  uint8_t i=0;
  bool HaveDigits = false;
  while (d>0)
  {
    uint8_t c = data/d;
    if ((c>0)||(HaveDigits)||(d==1))
    {
      if (c>9)
        c=9;
      pStr[i++] = '0'+c;
      data = data % d;
      HaveDigits = true;
    }
    else
    if (fillByte)
    {
      pStr[i++] = fillByte;
    }
    d /= 10;
  }
  return &pStr[i];
}

void ftpSendReply(char* buf, uint16_t code)
{
  // set three-digit reply code
  buf[0] = '0'+(code / 100);
  buf[1] = '0'+(code % 100)/10;
  buf[2] = '0'+(code % 10);
  buf[3] = ' ';

  // append plain message
  const char* msg;
  switch (code)
  {
    case 215: msg = FTP_DAN2;break;
    case 220: msg = FTP_WELCOME;break;
    case 221: msg = FTP_BYE;break;
    case 550: msg = FTP_NO_FILE;break;
    case 552: msg = FTP_TOO_LARGE;break;
    case 553: msg = FTP_BAD_FILENAME;break;
    default:
      msg = (code<=399) ? FTP_OK : FTP_FAILED;
  }

  uint8_t sz = 4;
  if (msg)
    sz+=strReadProgMem(&buf[sz], msg);
  FTP_CMD_REPLY(buf, sz);
}

// wait and accept for an incomming data connection
bool ftpAcceptDataConnection(bool Cleanup=false)
{
  uint16_t Timeout = (Cleanup) ? 10 : FTP_PASV_TIMEOUT;
  for (uint16_t i=1;i<Timeout;i++)
  {
    FtpDataClient = FtpDataServer.accept();
    if (FtpDataClient.connected())
    {
      FTP_DEBUG_PRINTLN(F("new"));
      if (Cleanup)
        FtpDataClient.stop();
      else
        FtpDataClient.setConnectionTimeout(5000);
      return true;
    }
    delay(1);
  }
  return false;
}

bool switchFile(uint8_t _unit, uint8_t _fileno1, uint8_t _fileno2)
{
  unit = _unit;
  unmount_drive();
  slot0_fileno = _fileno1;
  slot1_fileno = _fileno2;
  initialize_drive();
  CHECK_MEM(1010);
}

bool ftpSelectFile(uint8_t fileno)
{
  if (Ftp.Directory == DIR_ROOT)
    return false;
  switchFile((Ftp.Directory == DIR_SDCARD1) ? 0 : 0x80, fileno, fileno);
  return (Ftp.Directory == DIR_SDCARD1) ? slot0_state : slot1_state;
}

// obtains volume name and size
uint32_t getProdosVolumeInfo(uint8_t* buf)
{
  UINT br;
  uint32_t FileSize = f_size(&slotfile); // this is the size of the DOS file...

  // read PRODOS volume name from header
  uint8_t* ProdosHeader = &buf[FTP_BUF_SIZE-0x30];
  if ((f_lseek(&slotfile, 0x400) == FR_OK) &&
      (f_read(&slotfile, ProdosHeader, 0x30, &br) == FR_OK) &&
      (br == 0x30))
  {
    uint8_t len = ProdosHeader[4] ^ 0xf0; // top 4 bits must be set for PRODOS volume name lengths
    if (len <= 0xf) // valid length field?
    {          
      // write 15 ASCII characters with PRODOS volume name
      for (uint8_t i=0;i<15;i++)
      {
        char ascii = ProdosHeader[5+i];
        if ((i>=len)||(ascii<' ')||(ascii>'z'))
          ascii=' ';
        buf[21+i] = ascii;
      }

      // get PRODOS volume size
      uint32_t ProdosSize = ProdosHeader[0x2A];
      ProdosSize <<= 8;
      ProdosSize |= ProdosHeader[0x29];
      ProdosSize <<= 9; // x512 bytes per block
      // report PRODOS volume size (unless physical block device file is smaller)
      if (ProdosSize < FileSize)
        FileSize = ProdosSize;
    }
  }
  return FileSize;
}

void ftpHandleDirectory(char* buf)
{
  if (Ftp.Directory == DIR_ROOT)
  {
    strReadProgMem(buf, DIR_TEMPLATE); // get template for "SD1"
    memcpy(&buf[DIR_TEMPLATE_LENGTH], buf, DIR_TEMPLATE_LENGTH); // copy template for "SD2"
    buf[DIR_TEMPLATE_LENGTH*2-2-1] = '2'; // patch '1'=>'2' for SD2
    FtpDataClient.write(buf, DIR_TEMPLATE_LENGTH*2);
  }
  else
  {
    // iterate over possible file names: BLKDEVXX.PO
    for (uint8_t fno=FTP_FIRST_BLKDEV_FILE;fno<FTP_MAX_BLKDEV_FILES;fno++)
    {
      if (ftpSelectFile(fno))
      {
        strReadProgMem(buf, FILE_TEMPLATE);

        buf[FILE_TEMPLATE_LENGTH-7] = hex_digit(fno>>4);
        buf[FILE_TEMPLATE_LENGTH-6] = hex_digit(fno);

        // read PRODOS volume name from header
        uint32_t FileSize = getProdosVolumeInfo((uint8_t*) buf);

        // update file size
        strPrintInt(&buf[37], FileSize, 10000000, ' ');

        // send directory entry
        FtpDataClient.write(buf, FILE_TEMPLATE_LENGTH);
      }
    }
  }
}

// receive or send file data: return FTP reply code
uint16_t ftpHandleFileData(char* buf, uint8_t fileno, bool Read)
{
  if (!ftpSelectFile(fileno))
  {
    FTP_DEBUG_PRINTLN(F("nofile"));
    return 550;
  }

  // obtain ProDOS file size
  uint32_t ProDOSFileSize = getProdosVolumeInfo((uint8_t*) buf);

  int err = f_lseek(&slotfile, 0);
  if (err != FR_OK)
  {
    FTP_DEBUG_PRINTLN(F("badseek"));
    FTP_DEBUG_PRINTLN(err);
    return 551;
  }

  if (Read)
  {
    // read from disk and send to remote
    UINT br=1;
    // we only send the data for the ProDOS drive - the physical BLKDEVxx.PO file may be larger...
    while ((br > 0)&&(ProDOSFileSize > 0))
    {
      if ((f_read(&slotfile, buf, FTP_BUF_SIZE, &br)==FR_OK) && (br>0))
      {
        CHECK_MEM(1020);
        if (br > ProDOSFileSize)
          br = ProDOSFileSize;
        if (FtpDataClient.write(buf, br) != br)
        {
          return 426; // failed, connection aborted...
        }
        ProDOSFileSize -= br;
      }
      else
        br=0;
    }
  }
  else
  {
    // receive remote data and write to disk
    uint16_t TcpBytes=0;
    uint16_t BufOffset=0;
    long Timeout = 0;
    UINT bw;

    while ((Timeout == 0)||(((long) (millis()-Timeout))<0))
    {
      if (TcpBytes == 0)
      {
        TcpBytes = FtpDataClient.available();
        if (TcpBytes == 0)
        {
          if (!FtpDataClient.connected())
          {
            FTP_DEBUG_PRINTLN(F("discon"));
            FTP_DEBUG_PRINTLN(FtpDataClient.status());
            CHECK_MEM(1030);
            break;
          }
        }
      }
      if (TcpBytes > 0)
      {
        while (TcpBytes)
        {
          size_t sz = (BufOffset+TcpBytes > FTP_BUF_SIZE) ? FTP_BUF_SIZE-BufOffset : TcpBytes;
          int rd = FtpDataClient.read((uint8_t*) &buf[BufOffset], sz);
          CHECK_MEM(1040);
          if (rd>0)
          {
            // reset timeout when data was received
            Timeout = 0;
            if (f_eof(&slotfile))
            {
              // we will not write beyond the allocated file size
              return 552;
            }
            BufOffset += rd;
            TcpBytes-=rd;
            if (BufOffset >= FTP_BUF_SIZE)
            {
              // write block to disk
              int r = f_write(&slotfile, buf, FTP_BUF_SIZE, &bw);
              if (r != FR_OK)
              {
                FTP_DEBUG_PRINTLN(F("badwr"));
                return 552;
              }
              if (bw != FTP_BUF_SIZE)
              {
                FTP_DEBUG_PRINTLN(F("badwrsz"));
                return 552;
              }
              BufOffset=0;
            }
          }
        }
      }
      // calculate new timeout when necessary
      if (!Timeout)
      {
        Timeout = millis()+FTP_TRANSMIT_TIMEOUT;
      }
    }
    if (BufOffset>0)
    {
      int r = f_write(&slotfile, buf, FTP_BUF_SIZE, &bw);
      if ((r != FR_OK)||(bw!=FTP_BUF_SIZE))
      {
        return 552;
      }
    }
  }
  return 226; // file transfer successful
}

// check the requested file name - and get the file block device number
uint16_t getBlkDevFileNo(char* Data)
{
  uint8_t  digit = Data[6];
  Data[6] = 0;

  if (!strMatch(Data, "BLKDEV"))
  {
    return 255;
  }

  uint16_t fno = 0;
  for (uint8_t i=0;i<2;i++)
  {
    fno <<= 4;
    if ((digit>='0')&&(digit<='9'))
      digit -= '0';
    else
    if ((digit>='A')&&(digit<='F'))
      digit -= 'A'-10;
    fno |= digit;
    digit = Data[7];
  }
  
  return fno;
}

// simple command processing
void ftpCommand(char* buf, int8_t CmdId, char* Data)
{
  uint16_t ReplyCode = 0;
  CHECK_MEM(1100);
  switch(CmdId)
  {
    case FTP_CMD_USER: ReplyCode = 230; break; // Logged in.
    case FTP_CMD_SYST: ReplyCode = 215; break; // Report sys name
    case FTP_CMD_CWD:
      // change directory: we only support the root directory and SD1+SD2
      ReplyCode = 250; // file action OK
      if ((strMatch(Data, ".."))||(strMatch(Data, "/")))
        Ftp.Directory = DIR_ROOT;
      else
      {
        if (Data[0]=='/')
          Data++;
        if (strMatch(Data, "SD1"))
          Ftp.Directory = DIR_SDCARD1;
        else
        if (strMatch(Data, "SD2"))
          Ftp.Directory = DIR_SDCARD2;
        else
          ReplyCode = 550; // directory 'not found'...
      }
      break;
    case FTP_CMD_TYPE: ReplyCode = 200; break;
    case FTP_CMD_QUIT: ReplyCode = 221; break; // Bye.
    case FTP_CMD_PASV:
    {
      // sometimes, after commands have failed, we need to clean-up pending connections...
      while (ftpAcceptDataConnection(true));
      uint8_t sz = strReadProgMem(buf, PASSIVE_MODE_REPLY);
      char* s = &buf[sz];
      for (uint8_t i=0;i<6;i++)
      {
      if (i>0)
        *(s++) = ',';
      s = strPrintInt(s, FtpMacIpPortData[OFFSET_IP+i], 100, 0);
      }
      *(s++) = ')';
      FTP_CMD_REPLY(buf, (s-buf));
      break;
    }
    case FTP_CMD_LIST:
    case FTP_CMD_RETR:
    case FTP_CMD_STOR:
    {
      ftpSendReply(buf, 150);
      if (!ftpAcceptDataConnection())
      {
        ReplyCode = 425; // Can't open data connection.
      }
      else
      {
        if (CmdId == FTP_CMD_LIST)
        {
          ftpHandleDirectory(buf);
          ReplyCode = 226; // Listed.
        }
        else
        {
          uint16_t fno = getBlkDevFileNo(Data);
          if ((fno >= FTP_MAX_BLKDEV_FILES)||(fno<FTP_FIRST_BLKDEV_FILE))
            ReplyCode = 553; // file name not allowed
          else
            ReplyCode = ftpHandleFileData(buf, fno, (CmdId == FTP_CMD_RETR));
        }
        delay(10);
        FtpDataClient.stop();
      }
      break;
    }
    case FTP_CMD_CDUP:
      Ftp.Directory = DIR_ROOT;
      ReplyCode = 250; // Went to parent folder.
      break;
    case FTP_CMD_PWD:
    {
      strReadProgMem(buf, ROOT_DIR_TEMPLATE);
      if (Ftp.Directory == DIR_ROOT)
      {
        buf[6] = '\"';
        FTP_CMD_REPLY(buf, 7);
      }
      else
      {
        if (Ftp.Directory == DIR_SDCARD2)
          buf[ROOT_DIR_TEMPLATE_LENGTH-2] = '2';
        FTP_CMD_REPLY(buf, ROOT_DIR_TEMPLATE_LENGTH);
      }
      break;
    }
    default:
      ReplyCode = 502; // Unsupported command.
      break;
  }
  if (ReplyCode)
    ftpSendReply(buf, ReplyCode);
}

static void ftpInit()
{
  Ethernet.init(CS3);

#ifdef FTP_DEBUG
  Serial.begin(115200);
  FTP_DEBUG_PRINTLN(F("TinyTinyFTP"));
#endif

  // obtain IP address from the data structure
  IPAddress Ip(FtpMacIpPortData[OFFSET_IP],FtpMacIpPortData[OFFSET_IP+1],FtpMacIpPortData[OFFSET_IP+2],FtpMacIpPortData[OFFSET_IP+3]);

  Ethernet.begin(&FtpMacIpPortData[OFFSET_MAC], Ip);

  if (Ethernet.hardwareStatus() == EthernetNoHardware)
  {
    FTP_DEBUG_PRINTLN(F("Wiz5500 not found"));
    // no hardware: keep this module disabled
    Ftp.State = FTP_DISABLED;
    return;
  }

  if (Ethernet.linkStatus() == LinkOFF)
  {
    FTP_DEBUG_PRINTLN(F("Eth cable not connected"));
    // hardware but no connected cable: keep this module disabled
    Ftp.State = FTP_DISABLED;
    return;
  }

  // start the servers 
  FtpCmdServer.begin();
  FtpDataServer.begin();

#ifdef FTP_DEBUG
  FTP_DEBUG_PRINTLN(Ethernet.localIP());
#endif

  Ftp.State = FTP_INITIALIZED;
}

// FTP processing loop
void loopTinyFtp(void)
{
  char buf[FTP_BUF_SIZE];
  static int Throttle = 2000; // first FTP communication attempt after 2 seconds (Wiznet is slower than a bare Arduino w/o bootloader)
  
  if ((Ftp.State < 0)||
      ((Throttle != 0)&&((int) (millis()-Throttle) < 0)))
  {
    return;
  }

  if (Ftp.State == FTP_NOT_INITIALIZED)
  {
    ftpInit();
  }

  do
  {
    if (Ftp.State == FTP_INITIALIZED) // initialized but not connected
    {
      // accept incomming command connection
      FtpCmdClient = FtpCmdServer.accept();
      if (FtpCmdClient.connected())
      {
        FTP_DEBUG_PRINTLN(F("FTP con"));
        Ftp.State  = FTP_CONNECTED;
        // remember current Apple II volume configuration
        Ftp._file0 = slot0_fileno;
        Ftp._file1 = slot1_fileno;
        // FTP welcome
        ftpSendReply(buf, 220);
        // expect new command
        Ftp.CmdBytes = 0;
        // always start in root directory
        Ftp.Directory = DIR_ROOT;
        mmc_sync_write = 1;
      }
    }
    else
    if (Ftp.State == FTP_CONNECTED)
    {
      if (!FtpCmdClient.connected())
      {
        FTP_DEBUG_PRINTLN(F("FTP dis"));
        // give the remote client time to receive the data
        delay(10);
        FtpCmdClient.stop();
        Ftp.State = FTP_INITIALIZED;
        // select original Apple II volumes (just as if nothing happened...)
        switchFile(0x0, Ftp._file0, Ftp._file1);
        mmc_sync_write = 0;
      }
      else
      if (FtpCmdClient.available())
      {
        char c = FtpCmdClient.read();
        // Convert everything to upper case. So both, lower+upper case commands/filenames work.
        if ((c>='a')&&(c<='z'))
          c+='A'-'a';
        if (c == '\r')
        {
          // ignored
        }
        else
        if (Ftp.CmdBytes < 4) // FTP command length has a maximum of 4 bytes
        {
          Ftp.CmdData[Ftp.CmdBytes++] = (c=='\n') ? 0 : c;
          Ftp.CmdData[Ftp.CmdBytes] = 0;
          if ((c == '\n')||(Ftp.CmdBytes == 4))
            Ftp.CmdId = ftpGetCmdId(buf, Ftp.CmdData);
          if (c == '\n')
          {
            // execute command with no paramters
            ftpCommand(buf, Ftp.CmdId, "");
            Ftp.CmdBytes = 0;
          }
          Ftp.ParamBytes = 0;
        }
        else
        {
          // process FTP command parameter
          if (c==' ')
          {
            // ignored
          }
          else
          if (c == '\n') 
          {
            // command & parameter is complete
            Ftp.CmdData[Ftp.ParamBytes] = 0;
            ftpCommand(buf, Ftp.CmdId, Ftp.CmdData);
            Ftp.CmdBytes = 0;
            Ftp.ParamBytes = 0;
          }
          else
          if (Ftp.ParamBytes < sizeof(Ftp.CmdData)-1)
          {
              Ftp.CmdData[Ftp.ParamBytes++] = c;
          }
        }
      }
    }
    CHECK_MEM(1001);

  } while (Ftp.State == FTP_CONNECTED);

  // check every 100ms for incomming FTP connections
  Throttle = millis()+100;
}

#endif // USE_FTP