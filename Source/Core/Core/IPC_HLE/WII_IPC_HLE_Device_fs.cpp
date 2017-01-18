// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IPC_HLE/WII_IPC_HLE_Device_FileIO.h"
#include "Core/IPC_HLE/WII_IPC_HLE_Device_fs.h"

namespace IOS
{
namespace HLE
{
static bool IsValidWiiPath(const std::string& path)
{
  return path.compare(0, 1, "/") == 0;
}

CWII_IPC_HLE_Device_fs::CWII_IPC_HLE_Device_fs(u32 _DeviceID, const std::string& _rDeviceName)
    : IWII_IPC_HLE_Device(_DeviceID, _rDeviceName)
{
}

// ~1/1000th of a second is too short and causes hangs in Wii Party
// Play it safe at 1/500th
IPCCommandResult CWII_IPC_HLE_Device_fs::GetFSReply(const s32 return_value) const
{
  return {return_value, true, SystemTimers::GetTicksPerSecond() / 500};
}

IOSReturnCode CWII_IPC_HLE_Device_fs::Open(const IOSOpenRequest& request)
{
  // clear tmp folder
  {
    std::string Path = HLE_IPC_BuildFilename("/tmp");
    File::DeleteDirRecursively(Path);
    File::CreateDir(Path);
  }

  m_is_active = true;
  return IPC_SUCCESS;
}

// Get total filesize of contents of a directory (recursive)
// Only used for ES_GetUsage atm, could be useful elsewhere?
static u64 ComputeTotalFileSize(const File::FSTEntry& parentEntry)
{
  u64 sizeOfFiles = 0;
  for (const File::FSTEntry& entry : parentEntry.children)
  {
    if (entry.isDirectory)
      sizeOfFiles += ComputeTotalFileSize(entry);
    else
      sizeOfFiles += entry.size;
  }
  return sizeOfFiles;
}

IPCCommandResult CWII_IPC_HLE_Device_fs::IOCtlV(const IOSIOCtlVRequest& request)
{
  s32 return_value = IPC_SUCCESS;
  switch (request.request)
  {
  case IOCTLV_READ_DIR:
  {
    const std::string relative_path =
        Memory::GetString(request.in_vectors[0].address, request.in_vectors[0].size);

    if (!IsValidWiiPath(relative_path))
    {
      WARN_LOG(WII_IPC_FILEIO, "Not a valid path: %s", relative_path.c_str());
      return_value = FS_EINVAL;
      break;
    }

    // the Wii uses this function to define the type (dir or file)
    std::string DirName(HLE_IPC_BuildFilename(relative_path));

    INFO_LOG(WII_IPC_FILEIO, "FS: IOCTL_READ_DIR %s", DirName.c_str());

    if (!File::Exists(DirName))
    {
      WARN_LOG(WII_IPC_FILEIO, "FS: Search not found: %s", DirName.c_str());
      return_value = FS_ENOENT;
      break;
    }
    else if (!File::IsDirectory(DirName))
    {
      // It's not a directory, so error.
      // Games don't usually seem to care WHICH error they get, as long as it's <
      // Well the system menu CARES!
      WARN_LOG(WII_IPC_FILEIO, "\tNot a directory - return FS_EINVAL");
      return_value = FS_EINVAL;
      break;
    }

    File::FSTEntry entry = File::ScanDirectoryTree(DirName, false);

    // it is one
    if ((request.in_vectors.size() == 1) && (request.io_vectors.size() == 1))
    {
      size_t numFile = entry.children.size();
      INFO_LOG(WII_IPC_FILEIO, "\t%zu files found", numFile);

      Memory::Write_U32((u32)numFile, request.io_vectors[0].address);
    }
    else
    {
      for (File::FSTEntry& child : entry.children)
      {
        // Decode escaped invalid file system characters so that games (such as
        // Harry Potter and the Half-Blood Prince) can find what they expect.
        child.virtualName = Common::UnescapeFileName(child.virtualName);
      }

      std::sort(entry.children.begin(), entry.children.end(),
                [](const File::FSTEntry& one, const File::FSTEntry& two) {
                  return one.virtualName < two.virtualName;
                });

      u32 MaxEntries = Memory::Read_U32(request.in_vectors[0].address);

      memset(Memory::GetPointer(request.io_vectors[0].address), 0, request.io_vectors[0].size);

      size_t numFiles = 0;
      char* pFilename = (char*)Memory::GetPointer((u32)(request.io_vectors[0].address));

      for (size_t i = 0; i < entry.children.size() && i < MaxEntries; i++)
      {
        const std::string& FileName = entry.children[i].virtualName;

        strcpy(pFilename, FileName.c_str());
        pFilename += FileName.length();
        *pFilename++ = 0x00;  // termination
        numFiles++;

        INFO_LOG(WII_IPC_FILEIO, "\tFound: %s", FileName.c_str());
      }

      Memory::Write_U32((u32)numFiles, request.io_vectors[1].address);
    }

    return_value = IPC_SUCCESS;
  }
  break;

  case IOCTLV_GETUSAGE:
  {
    _dbg_assert_(WII_IPC_FILEIO, request.io_vectors.size() == 2);
    _dbg_assert_(WII_IPC_FILEIO, request.io_vectors[0].size == 4);
    _dbg_assert_(WII_IPC_FILEIO, request.io_vectors[1].size == 4);

    // this command sucks because it asks of the number of used
    // fsBlocks and inodes
    // It should be correct, but don't count on it...
    std::string relativepath =
        Memory::GetString(request.in_vectors[0].address, request.in_vectors[0].size);

    if (!IsValidWiiPath(relativepath))
    {
      WARN_LOG(WII_IPC_FILEIO, "Not a valid path: %s", relativepath.c_str());
      return_value = FS_EINVAL;
      break;
    }

    std::string path(HLE_IPC_BuildFilename(relativepath));
    u32 fsBlocks = 0;
    u32 iNodes = 0;

    INFO_LOG(WII_IPC_FILEIO, "IOCTL_GETUSAGE %s", path.c_str());
    if (File::IsDirectory(path))
    {
      // LPFaint99: After I found that setting the number of inodes to the number of children + 1
      // for the directory itself
      // I decided to compare with sneek which has the following 2 special cases which are
      // Copyright (C) 2009-2011  crediar http://code.google.com/p/sneek/
      if ((relativepath.compare(0, 16, "/title/00010001") == 0) ||
          (relativepath.compare(0, 16, "/title/00010005") == 0))
      {
        fsBlocks = 23;  // size is size/0x4000
        iNodes = 42;    // empty folders return a FileCount of 1
      }
      else
      {
        File::FSTEntry parentDir = File::ScanDirectoryTree(path, true);
        // add one for the folder itself
        iNodes = 1 + (u32)parentDir.size;

        u64 totalSize =
            ComputeTotalFileSize(parentDir);  // "Real" size, to be converted to nand blocks

        fsBlocks = (u32)(totalSize / (16 * 1024));  // one bock is 16kb
      }
      return_value = IPC_SUCCESS;

      INFO_LOG(WII_IPC_FILEIO, "FS: fsBlock: %i, iNodes: %i", fsBlocks, iNodes);
    }
    else
    {
      fsBlocks = 0;
      iNodes = 0;
      return_value = IPC_SUCCESS;
      WARN_LOG(WII_IPC_FILEIO, "FS: fsBlock failed, cannot find directory: %s", path.c_str());
    }

    Memory::Write_U32(fsBlocks, request.io_vectors[0].address);
    Memory::Write_U32(iNodes, request.io_vectors[1].address);
  }
  break;

  default:
    request.DumpUnknown(GetDeviceName(), LogTypes::WII_IPC_FILEIO);
    break;
  }

  return GetFSReply(return_value);
}

IPCCommandResult CWII_IPC_HLE_Device_fs::IOCtl(const IOSIOCtlRequest& request)
{
  Memory::Memset(request.buffer_out, 0, request.buffer_out_size);
  const s32 return_value = ExecuteCommand(request);
  return GetFSReply(return_value);
}

s32 CWII_IPC_HLE_Device_fs::ExecuteCommand(const IOSIOCtlRequest& request)
{
  switch (request.request)
  {
  case IOCTL_GET_STATS:
  {
    if (request.buffer_out_size < 0x1c)
      return -1017;

    WARN_LOG(WII_IPC_FILEIO, "FS: GET STATS - returning static values for now");

    NANDStat fs;

    // TODO: scrape the real amounts from somewhere...
    fs.BlockSize = 0x4000;
    fs.FreeUserBlocks = 0x5DEC;
    fs.UsedUserBlocks = 0x1DD4;
    fs.FreeSysBlocks = 0x10;
    fs.UsedSysBlocks = 0x02F0;
    fs.Free_INodes = 0x146B;
    fs.Used_Inodes = 0x0394;

    std::memcpy(Memory::GetPointer(request.buffer_out), &fs, sizeof(NANDStat));

    return IPC_SUCCESS;
  }
  break;

  case IOCTL_CREATE_DIR:
  {
    _dbg_assert_(WII_IPC_FILEIO, request.buffer_out_size == 0);
    u32 Addr = request.buffer_in;

    u32 OwnerID = Memory::Read_U32(Addr);
    Addr += 4;
    u16 GroupID = Memory::Read_U16(Addr);
    Addr += 2;
    const std::string wii_path = Memory::GetString(Addr, 64);
    if (!IsValidWiiPath(wii_path))
    {
      WARN_LOG(WII_IPC_FILEIO, "Not a valid path: %s", wii_path.c_str());
      return FS_EINVAL;
    }
    std::string DirName(HLE_IPC_BuildFilename(wii_path));
    Addr += 64;
    Addr += 9;  // owner attribs, permission
    u8 Attribs = Memory::Read_U8(Addr);

    INFO_LOG(WII_IPC_FILEIO, "FS: CREATE_DIR %s, OwnerID %#x, GroupID %#x, Attributes %#x",
             DirName.c_str(), OwnerID, GroupID, Attribs);

    DirName += DIR_SEP;
    File::CreateFullPath(DirName);
    _dbg_assert_msg_(WII_IPC_FILEIO, File::IsDirectory(DirName), "FS: CREATE_DIR %s failed",
                     DirName.c_str());

    return IPC_SUCCESS;
  }
  break;

  case IOCTL_SET_ATTR:
  {
    u32 Addr = request.buffer_in;

    u32 OwnerID = Memory::Read_U32(Addr);
    Addr += 4;
    u16 GroupID = Memory::Read_U16(Addr);
    Addr += 2;
    const std::string wii_path = Memory::GetString(Addr, 64);
    if (!IsValidWiiPath(wii_path))
    {
      WARN_LOG(WII_IPC_FILEIO, "Not a valid path: %s", wii_path.c_str());
      return FS_EINVAL;
    }
    std::string Filename = HLE_IPC_BuildFilename(wii_path);
    Addr += 64;
    u8 OwnerPerm = Memory::Read_U8(Addr);
    Addr += 1;
    u8 GroupPerm = Memory::Read_U8(Addr);
    Addr += 1;
    u8 OtherPerm = Memory::Read_U8(Addr);
    Addr += 1;
    u8 Attributes = Memory::Read_U8(Addr);
    Addr += 1;

    INFO_LOG(WII_IPC_FILEIO, "FS: SetAttrib %s", Filename.c_str());
    DEBUG_LOG(WII_IPC_FILEIO, "    OwnerID: 0x%08x", OwnerID);
    DEBUG_LOG(WII_IPC_FILEIO, "    GroupID: 0x%04x", GroupID);
    DEBUG_LOG(WII_IPC_FILEIO, "    OwnerPerm: 0x%02x", OwnerPerm);
    DEBUG_LOG(WII_IPC_FILEIO, "    GroupPerm: 0x%02x", GroupPerm);
    DEBUG_LOG(WII_IPC_FILEIO, "    OtherPerm: 0x%02x", OtherPerm);
    DEBUG_LOG(WII_IPC_FILEIO, "    Attributes: 0x%02x", Attributes);

    return IPC_SUCCESS;
  }
  break;

  case IOCTL_GET_ATTR:
  {
    _dbg_assert_msg_(WII_IPC_FILEIO, request.buffer_out_size == 76,
                     "    GET_ATTR needs an 76 bytes large output buffer but it is %i bytes large",
                     request.buffer_out_size);

    u32 OwnerID = 0;
    u16 GroupID = 0x3031;  // this is also known as makercd, 01 (0x3031) for nintendo and 08
                           // (0x3038) for MH3 etc
    const std::string wii_path = Memory::GetString(request.buffer_in, 64);
    if (!IsValidWiiPath(wii_path))
    {
      WARN_LOG(WII_IPC_FILEIO, "Not a valid path: %s", wii_path.c_str());
      return FS_EINVAL;
    }
    std::string Filename = HLE_IPC_BuildFilename(wii_path);
    u8 OwnerPerm = 0x3;    // read/write
    u8 GroupPerm = 0x3;    // read/write
    u8 OtherPerm = 0x3;    // read/write
    u8 Attributes = 0x00;  // no attributes
    if (File::IsDirectory(Filename))
    {
      INFO_LOG(WII_IPC_FILEIO, "FS: GET_ATTR Directory %s - all permission flags are set",
               Filename.c_str());
    }
    else
    {
      if (File::Exists(Filename))
      {
        INFO_LOG(WII_IPC_FILEIO, "FS: GET_ATTR %s - all permission flags are set",
                 Filename.c_str());
      }
      else
      {
        INFO_LOG(WII_IPC_FILEIO, "FS: GET_ATTR unknown %s", Filename.c_str());
        return FS_ENOENT;
      }
    }

    // write answer to buffer
    if (request.buffer_out_size == 76)
    {
      u32 Addr = request.buffer_out;
      Memory::Write_U32(OwnerID, Addr);
      Addr += 4;
      Memory::Write_U16(GroupID, Addr);
      Addr += 2;
      memcpy(Memory::GetPointer(Addr), Memory::GetPointer(request.buffer_in), 64);
      Addr += 64;
      Memory::Write_U8(OwnerPerm, Addr);
      Addr += 1;
      Memory::Write_U8(GroupPerm, Addr);
      Addr += 1;
      Memory::Write_U8(OtherPerm, Addr);
      Addr += 1;
      Memory::Write_U8(Attributes, Addr);
      Addr += 1;
    }

    return IPC_SUCCESS;
  }
  break;

  case IOCTL_DELETE_FILE:
  {
    _dbg_assert_(WII_IPC_FILEIO, request.buffer_out_size == 0);
    int Offset = 0;

    const std::string wii_path = Memory::GetString(request.buffer_in + Offset, 64);
    if (!IsValidWiiPath(wii_path))
    {
      WARN_LOG(WII_IPC_FILEIO, "Not a valid path: %s", wii_path.c_str());
      return FS_EINVAL;
    }
    std::string Filename = HLE_IPC_BuildFilename(wii_path);
    Offset += 64;
    if (File::Delete(Filename))
    {
      INFO_LOG(WII_IPC_FILEIO, "FS: DeleteFile %s", Filename.c_str());
    }
    else if (File::DeleteDir(Filename))
    {
      INFO_LOG(WII_IPC_FILEIO, "FS: DeleteDir %s", Filename.c_str());
    }
    else
    {
      WARN_LOG(WII_IPC_FILEIO, "FS: DeleteFile %s - failed!!!", Filename.c_str());
    }

    return IPC_SUCCESS;
  }
  break;

  case IOCTL_RENAME_FILE:
  {
    _dbg_assert_(WII_IPC_FILEIO, request.buffer_out_size == 0);
    int Offset = 0;

    const std::string wii_path = Memory::GetString(request.buffer_in + Offset, 64);
    if (!IsValidWiiPath(wii_path))
    {
      WARN_LOG(WII_IPC_FILEIO, "Not a valid path: %s", wii_path.c_str());
      return FS_EINVAL;
    }
    std::string Filename = HLE_IPC_BuildFilename(wii_path);
    Offset += 64;

    const std::string wii_path_rename = Memory::GetString(request.buffer_in + Offset, 64);
    if (!IsValidWiiPath(wii_path_rename))
    {
      WARN_LOG(WII_IPC_FILEIO, "Not a valid path: %s", wii_path_rename.c_str());
      return FS_EINVAL;
    }
    std::string FilenameRename = HLE_IPC_BuildFilename(wii_path_rename);
    Offset += 64;

    // try to make the basis directory
    File::CreateFullPath(FilenameRename);

    // if there is already a file, delete it
    if (File::Exists(Filename) && File::Exists(FilenameRename))
    {
      File::Delete(FilenameRename);
    }

    // finally try to rename the file
    if (File::Rename(Filename, FilenameRename))
    {
      INFO_LOG(WII_IPC_FILEIO, "FS: Rename %s to %s", Filename.c_str(), FilenameRename.c_str());
    }
    else
    {
      ERROR_LOG(WII_IPC_FILEIO, "FS: Rename %s to %s - failed", Filename.c_str(),
                FilenameRename.c_str());
      return FS_ENOENT;
    }

    return IPC_SUCCESS;
  }
  break;

  case IOCTL_CREATE_FILE:
  {
    _dbg_assert_(WII_IPC_FILEIO, request.buffer_out_size == 0);

    u32 Addr = request.buffer_in;
    u32 OwnerID = Memory::Read_U32(Addr);
    Addr += 4;
    u16 GroupID = Memory::Read_U16(Addr);
    Addr += 2;
    const std::string wii_path = Memory::GetString(Addr, 64);
    if (!IsValidWiiPath(wii_path))
    {
      WARN_LOG(WII_IPC_FILEIO, "Not a valid path: %s", wii_path.c_str());
      return FS_EINVAL;
    }
    std::string Filename(HLE_IPC_BuildFilename(wii_path));
    Addr += 64;
    u8 OwnerPerm = Memory::Read_U8(Addr);
    Addr++;
    u8 GroupPerm = Memory::Read_U8(Addr);
    Addr++;
    u8 OtherPerm = Memory::Read_U8(Addr);
    Addr++;
    u8 Attributes = Memory::Read_U8(Addr);
    Addr++;

    INFO_LOG(WII_IPC_FILEIO, "FS: CreateFile %s", Filename.c_str());
    DEBUG_LOG(WII_IPC_FILEIO, "    OwnerID: 0x%08x", OwnerID);
    DEBUG_LOG(WII_IPC_FILEIO, "    GroupID: 0x%04x", GroupID);
    DEBUG_LOG(WII_IPC_FILEIO, "    OwnerPerm: 0x%02x", OwnerPerm);
    DEBUG_LOG(WII_IPC_FILEIO, "    GroupPerm: 0x%02x", GroupPerm);
    DEBUG_LOG(WII_IPC_FILEIO, "    OtherPerm: 0x%02x", OtherPerm);
    DEBUG_LOG(WII_IPC_FILEIO, "    Attributes: 0x%02x", Attributes);

    // check if the file already exist
    if (File::Exists(Filename))
    {
      INFO_LOG(WII_IPC_FILEIO, "\tresult = FS_EEXIST");
      return FS_EEXIST;
    }

    // create the file
    File::CreateFullPath(Filename);  // just to be sure
    bool Result = File::CreateEmptyFile(Filename);
    if (!Result)
    {
      ERROR_LOG(WII_IPC_FILEIO, "CWII_IPC_HLE_Device_fs: couldn't create new file");
      PanicAlert("CWII_IPC_HLE_Device_fs: couldn't create new file");
      return FS_EINVAL;
    }

    INFO_LOG(WII_IPC_FILEIO, "\tresult = IPC_SUCCESS");
    return IPC_SUCCESS;
  }
  break;
  case IOCTL_SHUTDOWN:
  {
    INFO_LOG(WII_IPC_FILEIO, "Wii called Shutdown()");
    // TODO: stop emulation
  }
  break;
  default:
    request.DumpUnknown(GetDeviceName(), LogTypes::WII_IPC_FILEIO);
  }

  return FS_EINVAL;
}

void CWII_IPC_HLE_Device_fs::DoState(PointerWrap& p)
{
  DoStateShared(p);

  // handle /tmp

  std::string Path = File::GetUserPath(D_SESSION_WIIROOT_IDX) + "/tmp";
  if (p.GetMode() == PointerWrap::MODE_READ)
  {
    File::DeleteDirRecursively(Path);
    File::CreateDir(Path);

    // now restore from the stream
    while (1)
    {
      char type = 0;
      p.Do(type);
      if (!type)
        break;
      std::string filename;
      p.Do(filename);
      std::string name = Path + DIR_SEP + filename;
      switch (type)
      {
      case 'd':
      {
        File::CreateDir(name);
        break;
      }
      case 'f':
      {
        u32 size = 0;
        p.Do(size);

        File::IOFile handle(name, "wb");
        char buf[65536];
        u32 count = size;
        while (count > 65536)
        {
          p.DoArray(buf);
          handle.WriteArray(&buf[0], 65536);
          count -= 65536;
        }
        p.DoArray(&buf[0], count);
        handle.WriteArray(&buf[0], count);
        break;
      }
      }
    }
  }
  else
  {
    // recurse through tmp and save dirs and files

    File::FSTEntry parentEntry = File::ScanDirectoryTree(Path, true);
    std::deque<File::FSTEntry> todo;
    todo.insert(todo.end(), parentEntry.children.begin(), parentEntry.children.end());

    while (!todo.empty())
    {
      File::FSTEntry& entry = todo.front();
      std::string name = entry.physicalName;
      name.erase(0, Path.length() + 1);
      char type = entry.isDirectory ? 'd' : 'f';
      p.Do(type);
      p.Do(name);
      if (entry.isDirectory)
      {
        todo.insert(todo.end(), entry.children.begin(), entry.children.end());
      }
      else
      {
        u32 size = (u32)entry.size;
        p.Do(size);

        File::IOFile handle(entry.physicalName, "rb");
        char buf[65536];
        u32 count = size;
        while (count > 65536)
        {
          handle.ReadArray(&buf[0], 65536);
          p.DoArray(buf);
          count -= 65536;
        }
        handle.ReadArray(&buf[0], count);
        p.DoArray(&buf[0], count);
      }
      todo.pop_front();
    }

    char type = 0;
    p.Do(type);
  }
}
}  // namespace HLE
}  // namespace IOS
