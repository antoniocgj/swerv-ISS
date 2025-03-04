//
// SPED-License-Identifier: GPL-3.0-or-later
// Copyright 2018 Western Digital Corporation or its affiliates.
// 
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
// 
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
// 
// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.
//

#pragma once

#include <unordered_map>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <type_traits>
#include <cassert>

namespace WdRiscv
{

  template <typename URV>
  class Hart;

  /// Page attributes.
  struct PageAttribs
  {
    PageAttribs()
      : read_(false), write_(false), exec_(false),
	reg_(false), iccm_(false), dccm_(false)
    { }

    /// Set all attributes to given flag.
    void setAll(bool flag)
    {
      read_ = flag;
      write_ = flag;
      exec_ = flag;
      reg_ = flag;
      iccm_ = flag;
      dccm_ = flag;
    }

    /// Mark page as writable/non-writable.
    void setWrite(bool flag)
    { write_ = flag; }

    /// Mark/unmark page as usable for instruction fetch.
    void setExec(bool flag)
    { exec_ = flag; }

    /// Mark/unmark page as readable.
    void setRead(bool flag)
    { read_ = flag; }

    /// Mark/unmark page as usable for memory-mapped registers.
    void setMemMappedReg(bool flag)
    { reg_ = flag; }

    /// Mark page as belonging to an ICCM region.
    void setIccm(bool flag)
    { iccm_ = flag; }

    /// Mark page as belonging to a DCCM region.
    void setDccm(bool flag)
    { dccm_ = flag; }

    /// Return true if page can be used for instruction fetch. Fetch
    /// will still fail if page is not mapped.
    bool isExec() const
    { return exec_; }

    /// Return true if page can be used for data access (load/store
    /// instructions). Access will fail is page is not mapped. Write
    /// access (store instructions) will fail if page is not
    /// writable.
    bool isRead() const
    { return read_; }

    /// Return true if page is writable (write will still fail if
    /// page is not mapped).
    bool isWrite() const
    { return write_; }

    /// True if page belongs to an ICCM region.
    bool isIccm() const
    { return iccm_; }

    /// True if page belongs to a DCCM region.
    bool isDccm() const
    { return dccm_; }

    /// True if page is marked for memory-mapped registers.
    bool isMemMappedReg() const
    { return reg_; }

    /// Return true if page is external to the core.
    bool isExternal() const
    { return not dccm_ and not reg_; }

    /// True if page is mapped (usable).
    bool isMapped() const
    { return read_ or write_ or exec_; }

    bool read_            : 1; // True if page is readable.
    bool write_           : 1; // True if page is writable.
    bool exec_            : 1; // True if page can be used for fetching insts.
    bool reg_             : 1; // True if page has memory mapped registers.
    bool iccm_            : 1; // True if page is in an ICCM section.
    bool dccm_            : 1; // True if page is in a DCCM section.

    // When page size is small (64-bytes), the number of pages becomes
    // very large. Using packed attribute helps reduce memory usage.
  } __attribute__((packed));


  /// Location and size of an ELF file symbol.
  struct ElfSymbol
  {
    ElfSymbol(size_t addr = 0, size_t size = 0)
      : addr_(addr), size_(size)
    { }

    size_t addr_ = 0;
    size_t size_ = 0;
  };


  /// Model physical memory of system.
  class Memory
  {
  public:

    friend class Hart<uint32_t>;
    friend class Hart<uint64_t>;

    /// Constructor: define a memory of the given size initialized to
    /// zero. Given memory size (byte count) must be a multiple of 4
    /// otherwise, it is truncated to a multiple of 4. The memory
    /// is partitioned into regions according to the region size which
    /// must be a power of 2.
    Memory(size_t size, size_t pageSize = 4*1024,
	   size_t regionSize = 256*1024*1024);

    /// Destructor.
    ~Memory();

    /// Define number of hardware threads for LR/SC. FIX: put this in
    /// constructor.
    void setHartCount(unsigned count)
    { reservations_.resize(count); lastWriteData_.resize(count); }

    /// Return memory size in bytes.
    size_t size() const
    { return size_; }

    /// Read an unsigned integer value of type T from memory at the
    /// given address into value. Return true on success. Return false
    /// if any of the requested bytes is out of memory bounds or fall
    /// in unmapped memory or if the read crosses memory regions of
    /// different attributes.
    template <typename T>
    bool read(size_t address, T& value) const
    {
      PageAttribs attrib = getAttrib(address);
      if (not attrib.isRead())
	return false;

      if (address & (sizeof(T) - 1))  // If address is misaligned
	{
	  size_t page = getPageStartAddr(address);
	  size_t page2 = getPageStartAddr(address + sizeof(T) - 1);
	  if (page != page2)
	    {
	      // Read crosses page boundary: Check next page.
	      PageAttribs attrib2 = getAttrib(address + sizeof(T));
	      if (not attrib2.isRead())
		return false;
	      if (attrib.isDccm() != attrib2.isDccm())
		return false;  // Cannot cross a DCCM boundary.
	      if (attrib.isMemMappedReg() != attrib2.isMemMappedReg())
		return false;  // Cannot cross a PIC boundary.
	    }
	}

      // Memory mapped region accessible only with word-size read.
      if constexpr (sizeof(T) == 4)
        {
	  if (attrib.isMemMappedReg())
	    return readRegister(address, value);
	}
      else if (attrib.isMemMappedReg())
	return false;

      value = *(reinterpret_cast<const T*>(data_ + address));
      return true;
    }

    /// Read byte from given address into value. Return true on
    /// success.  Return false if address is out of bounds.
    bool readByte(size_t address, uint8_t& value) const
    {
      PageAttribs attrib = getAttrib(address);
      if (not attrib.isRead())
	return false;

      if (attrib.isMemMappedReg())
	return false; // Only word access allowed to memory mapped regs.

      value = data_[address];
      return true;
    }

    /// Read half-word (2 bytes) from given address into value. See
    /// read method.
    bool readHalfWord(size_t address, uint16_t& value) const
    { return read(address, value); }

    /// Read word (4 bytes) from given address into value. See read
    /// method.
    bool readWord(size_t address, uint32_t& value) const
    { return read(address, value); }

    /// Read a double-word (8 bytes) from given address into
    /// value. See read method.
    bool readDoubleWord(size_t address, uint64_t& value) const
    { return read(address, value); }

    /// On a unified memory model, this is the same as readHalfWord.
    /// On a split memory model, this will taken an exception if the
    /// target address is not in instruction memory.
    bool readInstHalfWord(size_t address, uint16_t& value) const
    {
      PageAttribs attrib = getAttrib(address);
      if (attrib.isExec())
	{
	  if (address & 1)
	    {
	      size_t page = getPageStartAddr(address);
	      size_t page2 = getPageStartAddr(address + 1);
	      if (page != page2)
		{
		  // Instruction crosses page boundary: Check next page.
		  PageAttribs attrib2 = getAttrib(address + 1);
		  if (not attrib2.isExec())
		    return false;
		  if (attrib.isIccm() != attrib2.isIccm())
		    return false;  // Cannot cross an ICCM boundary.
		}
	    }

	  value = *(reinterpret_cast<const uint16_t*>(data_ + address));
	  return true;
	}
      return false;
    }

    /// On a unified memory model, this is the same as readWord.
    /// On a split memory model, this will taken an exception if the
    /// target address is not in instruction memory.
    bool readInstWord(size_t address, uint32_t& value) const
    {
      PageAttribs attrib = getAttrib(address);
      if (attrib.isExec())
	{
	  if (address & 3)
	    {
	      size_t page = getPageStartAddr(address);
	      size_t page2 = getPageStartAddr(address + 3);
	      if (page != page2)
		{
		  // Instruction crosses page boundary: Check next page.
		  PageAttribs attrib2 = getAttrib(address + 3);
		  if (not attrib2.isExec())
		    return false;
		  if (attrib.isIccm() != attrib2.isIccm())
		    return false;  // Cannot cross a ICCM boundary.
		}
	    }

	  value = *(reinterpret_cast<const uint32_t*>(data_ + address));
	  return true;
	}
	return false;
    }

    /// Return true if write will be successful if tried. Do not
    /// write.  Change value to the maksed value if write is to a
    /// memory mapped register.
    template <typename T>
    bool checkWrite(size_t address, T& value)
    {
      PageAttribs attrib1 = getAttrib(address);
      bool dccm1 = attrib1.isDccm();
      if (not attrib1.isWrite())
	return false;

      if (address & (sizeof(T) - 1))  // If address is misaligned
	{
	  size_t page = getPageStartAddr(address);
	  size_t page2 = getPageStartAddr(address + sizeof(T) - 1);
	  if (page != page2)
	    {
	      // Write crosses page boundary: Check next page.
	      PageAttribs attrib2 = getAttrib(address + sizeof(T));
	      if (not attrib2.isWrite())
		return false;
	      if (dccm1 != attrib2.isDccm())
		return false;  // Cannot cross a DCCM boundary.
	      if (attrib1.isMemMappedReg() != attrib2.isMemMappedReg())
		return false;  // Cannot cross a PIC boundary.
	    }
	}

      // Memory mapped region accessible only with word-size write.
      if constexpr (sizeof(T) == 4)
        {
	  if (attrib1.isMemMappedReg() and (address & 3) != 0)
	    return false;
	  value = doRegisterMasking(address, value);
	}
      else if (attrib1.isMemMappedReg())
	return false;

      return true;
    }

    /// Write given unsigned integer value of type T into memory
    /// starting at the given address. Return true on success. Return
    /// false if any of the target memory bytes are out of bounds or
    /// fall in inaccessible regions or if the write crosses memory
    /// region of different attributes.
    template <typename T>
    bool write(unsigned localHartId, size_t address, T value)
    {
      PageAttribs attrib1 = getAttrib(address);
      bool dccm1 = attrib1.isDccm();
      if (not attrib1.isWrite())
	return false;

      if (address & (sizeof(T) - 1))  // If address is misaligned
	{
	  size_t page = getPageStartAddr(address);
	  size_t page2 = getPageStartAddr(address + sizeof(T) - 1);
	  if (page != page2)
	    {
	      // Write crosses page boundary: Check next page.
	      PageAttribs attrib2 = getAttrib(address + sizeof(T));
	      if (not attrib2.isWrite())
		return false;
	      if (dccm1 != attrib2.isDccm())
		return false;  // Cannot cross a DCCM boundary.
	      if (attrib1.isMemMappedReg() != attrib2.isMemMappedReg())
		return false;  // Cannot cross a PIC boundary.
	    }
	}

      // Memory mapped region accessible only with word-size write.
      if constexpr (sizeof(T) == 4)
        {
	  if (attrib1.isMemMappedReg())
	    return writeRegister(localHartId, address, value);
	}
      else if (attrib1.isMemMappedReg())
	return false;

      auto& lwd = lastWriteData_.at(localHartId);

      lwd.prevValue_ = *(reinterpret_cast<T*>(data_ + address));
      *(reinterpret_cast<T*>(data_ + address)) = value;
      lwd.size_ = sizeof(T);
      lwd.addr_ = address;
      lwd.value_ = value;
      return true;
    }

    /// Write byte to given address. Return true on success. Return
    /// false if address is out of bounds or is not writable.
    bool writeByte(unsigned localHartId, size_t address, uint8_t value)
    {
      PageAttribs attrib = getAttrib(address);
      if (not attrib.isWrite())
	return false;

      if (attrib.isMemMappedReg())
	return false;  // Only word access allowed to memory mapped regs.

      auto& lwd = lastWriteData_.at(localHartId);
      lwd.prevValue_ = *(data_ + address);

      data_[address] = value;

      lwd.size_ = 1;
      lwd.addr_ = address;
      lwd.value_ = value;
      return true;
    }

    /// Write half-word (2 bytes) to given address. Return true on
    /// success. Return false if address is out of bounds or is not
    /// writable.
    bool writeHalfWord(unsigned localHartId, size_t address, uint16_t value)
    { return write(localHartId, address, value); }

    /// Read word (4 bytes) from given address into value. Return true
    /// on success.  Return false if address is out of bounds or is
    /// not writable.
    bool writeWord(unsigned localHartId, size_t address, uint32_t value)
    { return write(localHartId, address, value); }

    /// Read a double-word (8 bytes) from given address into
    /// value. Return true on success. Return false if address is out
    /// of bounds.
    bool writeDoubleWord(unsigned localHartId, size_t address, uint64_t value)
    { return write(localHartId, address, value); }

    /// Load the given hex file and set memory locations accordingly.
    /// Return true on success. Return false if file does not exists,
    /// cannot be opened or contains malformed data.
    /// File format: A line either contains @address where address
    /// is a hexadecimal memory address or one or more space separated
    /// tokens each consisting of two hexadecimal digits.
    bool loadHexFile(const std::string& file);

    /// Load the given ELF file and set memory locations accordingly.
    /// Return true on success. Return false if file does not exists,
    /// cannot be opened or contains malformed data, or if it contains
    /// data incompatible with the given register width (32 or 64). If
    /// successful, set entryPoint to the entry point of the loaded
    /// file and end to the address past that of the loaded byte with
    /// the largest address. Extract symbol names and corresponding
    /// addresses and sizes into the memory symbols map.
    bool loadElfFile(const std::string& file, unsigned registerWidth,
		     size_t& entryPoint, size_t& end);

    /// Locate the given ELF symbol (symbols are collected for every
    /// loaded ELF file) returning true if symbol is found and false
    /// otherwise. Set value to the corresponding value if symbol is
    /// found.
    bool findElfSymbol(const std::string& symbol, ElfSymbol& value) const;

    /// Locate the ELF function cotaining the give address returning true
    /// on success and false on failure.  If successful set name to the
    /// corresponding function name and symbol to the corresponding symbol
    /// value.
    bool findElfFunction(size_t addr, std::string& name, ElfSymbol& value) const;

    /// Print the ELF symbols on the given stream. Output format:
    /// <name> <value>
    void printElfSymbols(std::ostream& out) const;

    /// Enable/disable errors on unmapped memory when loading ELF files.
    void checkUnmappedElf(bool flag)
    { checkUnmappedElf_ = flag; }

    /// Return the min and max addresses corresponding to the segments
    /// in the given ELF file. Return true on success and false if
    /// the ELF file does not exist or cannot be read (in which
    /// case min and max address are left unmodified).
    static bool getElfFileAddressBounds(const std::string& file,
					size_t& minAddr, size_t& maxAddr);

    /// Copy data from the given memory into this memory. If the two
    /// memories have different sizes then copy data from location
    /// zero up to n-1 where n is the minimum of the sizes.
    void copy(const Memory& other);

    /// Return true if given path corresponds to an ELF file and set
    /// the given flags according to the contents of the file.  Return
    /// false leaving the flags unmodified if file does not exist,
    /// cannot be read, or is not an ELF file.
    static bool checkElfFile(const std::string& path, bool& is32bit,
			     bool& is64bit, bool& isRiscv);

    /// Return true if given symbol is present in the given ELF file.
    static bool isSymbolInElfFile(const std::string& path,
				  const std::string& target);

  protected:

    /// Same as write but effects not recorded in last-write info.
    template <typename T>
    bool poke(size_t address, T value)
    {
      PageAttribs attrib = getAttrib(address);
      if (not attrib.isMapped())
	return false;

      size_t pageEnd = getPageStartAddr(address) + pageSize_;
      if (address + sizeof(T) > pageEnd)
	{
	  // Write crosses page boundary: Check next page.
	  PageAttribs attrib2 = getAttrib(address + sizeof(T));
	  if (not attrib2.isMapped())
	    return false;
	}

      // Memory mapped region accessible only with word-size poke.
      if constexpr (sizeof(T) == 4)
        {
	  if (attrib.isMemMappedReg())
	    {
	      if ((address & 3) != 0)
		return false;  // Address must be word-aligned.
	    }
	}
      else if (attrib.isMemMappedReg())
	return false;

      *(reinterpret_cast<T*>(data_ + address)) = value;
      return true;
    }

    /// Same as writeByte but effects are not record in last-write info.
    bool pokeByte(size_t address, uint8_t value)
    {
      PageAttribs attrib = getAttrib(address);
      if (not attrib.isMapped())
	return false;

      if (attrib.isMemMappedReg())
	return false;  // Only word access allowed to memory mapped regs.

      data_[address] = value;
      return true;
    }

    /// Write byte to given address. Return true on success. Return
    /// false if address is not mapped. This is used to initialize
    /// memory.
    bool writeByteNoAccessCheck(size_t address, uint8_t value);

    /// Set addr to the address of the last write and value to the
    /// corresponding value and return the size of that write. Return
    /// 0 if no write since the most recent clearLastWriteInfo in
    /// which case addr and value are not modified.
    unsigned getLastWriteNewValue(unsigned localHartId, size_t& addr, uint64_t& value) const
    {
      const auto& lwd = lastWriteData_.at(localHartId);
      if (lwd.size_)
	{
	  addr = lwd.addr_;
	  value = lwd.value_;
	}
      return lwd.size_;
    }

    /// Set addr to the address of the last write and value to the
    /// corresponding previous value (value before last write) and
    /// return the size of that write. Return 0 if no write since the
    /// most recent clearLastWriteInfo in which case addr and value
    /// are not modified.
    unsigned getLastWriteOldValue(unsigned localHartId, size_t& addr, uint64_t& value) const
    {
      auto& lwd = lastWriteData_.at(localHartId);
      if (lwd.size_)
	{
	  addr = lwd.addr_;
	  value = lwd.value_;
	}
      return lwd.size_;
    }

    /// Set value to the memory value before last write.  Return 0 if
    /// no write since the most recent clearLastWriteInfo in which
    /// case value is not modified.
    unsigned getLastWriteOldValue(unsigned localHartId, uint64_t& value) const
    {
      auto& lwd = lastWriteData_.at(localHartId);
      if (lwd.size_)
	value = lwd.prevValue_;
      return lwd.size_;
    }

    /// Clear the information associated with last write.
    void clearLastWriteInfo(unsigned localHartId)
    {
      auto& lwd = lastWriteData_.at(localHartId);
      lwd.size_ = 0;
    }

    /// Return the page size.
    size_t pageSize() const
    { return pageSize_; }

    /// Return the number of the page containing the given address.
    size_t getPageIx(size_t addr) const
    { return addr >> pageShift_; }

    /// Return the attribute of the page containing given address.
    PageAttribs getAttrib(size_t addr) const
    {
      size_t ix = getPageIx(addr);
      return ix < attribs_.size() ? attribs_[ix] : PageAttribs();
    }

    /// Return start address of page containing given address.
    size_t getPageStartAddr(size_t addr) const
    { return (addr >> pageShift_) << pageShift_; }

    /// Return true if CCM (iccm or dccm) configuration defined by
    /// region/offset/size is valid. Return false otherwise. Tag
    /// parameter ("iccm"/"dccm") is used with error messages.
    bool checkCcmConfig(const std::string& tag, size_t region, size_t offset,
			size_t size) const;

    /// Complain if CCM (iccm or dccm) defined by region/offset/size
    /// overlaps a previously defined CCM area. Return true if all is
    /// well (no overlap).
    bool checkCcmOverlap(const std::string& tag, size_t region, size_t offset,
			 size_t size, bool iccm, bool dccm, bool pic);

    /// Define instruction closed coupled memory (in core instruction memory).
    bool defineIccm(size_t region, size_t offset, size_t size);

    /// Define data closed coupled memory (in core data memory).
    bool defineDccm(size_t region, size_t offset, size_t size);

    /// Define region for memory mapped registers. Return true on
    /// success and false if offset or size are not properly aligned
    /// or sized.
    bool defineMemoryMappedRegisterRegion(size_t region, size_t offset,
					  size_t size);

    /// Reset (to zero) all memory mapped registers.
    void resetMemoryMappedRegisters();

    /// Define write mask for a memory-mapped register with given
    /// index and register-offset within the given region and region-offset.
    /// Address of memory associated with register is:
    ///   region*256M + regionOffset + registerBlockOffset + registerIx*4.
    /// Return true on success and false if the region (index) is not
    /// valid or if the region is not mapped of if the region was not
    /// defined for memory mapped registers or if the register address
    /// is out of bounds.
    bool defineMemoryMappedRegisterWriteMask(size_t region,
					     size_t regionOffset,
					     size_t registerBlockOffset,
					     size_t registerIx,
					     uint32_t mask);

    /// Called after memory is configured to refine memory access to
    /// sections of regions containing ICCM, DCCM or PIC-registers.
    void finishCcmConfig();

    /// Read a memory mapped register.
    bool readRegister(size_t addr, uint32_t& value) const
    {
      if ((addr & 3) != 0)
	return false;  // Address must be workd-aligned.
      value = *(reinterpret_cast<const uint32_t*>(data_ + addr));
      return true;
    }

    /// Return memory mapped mask associated with the word containing
    /// the given address. Return all 1 if given address is not a
    /// memory mapped register.
    uint32_t getMemoryMappedMask(size_t addr) const
    {
      uint32_t mask = ~ uint32_t(0);
      if (masks_.empty())
	return mask;

      size_t pageIx = getPageIx(addr);
      auto& pageMasks = masks_.at(pageIx);
      if (pageMasks.empty())
	return mask;

      size_t wordIx = (addr - getPageStartAddr(addr)) / 4;
      mask = pageMasks.at(wordIx);
      return mask;
    }

    /// Perform masking for a write to a memory mapped register.
    /// Return masked value.
    uint32_t doRegisterMasking(size_t addr, uint32_t value) const
    {
      uint32_t mask = getMemoryMappedMask(addr);
      value = value & mask;
      return value;
    }

    /// Write a memory mapped register.
    bool writeRegister(unsigned localHartId, size_t addr, uint32_t value)
    {
      if ((addr & 3) != 0)
	return false;  // Address must be word-aligned.

      value = doRegisterMasking(addr, value);

      auto& lwd = lastWriteData_.at(localHartId);
      lwd.prevValue_ = *(reinterpret_cast<uint32_t*>(data_ + addr));

      *(reinterpret_cast<uint32_t*>(data_ + addr)) = value;

      lwd.size_ = 4;
      lwd.addr_ = addr;
      lwd.value_ = value;
      return true;
    }

    /// Return the number of the 256-mb region containing given address.
    size_t getRegionIndex(size_t addr) const
    { return (addr >> regionShift_) & regionMask_; }

    /// Return true if given address is in a mapped page.
    bool isAddrMapped(size_t addr) const
    { return getAttrib(addr).isMapped(); }

    /// Return true if given address is in a readable page.
    bool isAddrReadable(size_t addr) const
    { return getAttrib(addr).isRead(); }

    /// Return true if page of given address is in data closed coupled
    /// memory.
    bool isAddrInDccm(size_t addr) const
    { return getAttrib(addr).isDccm(); }

    /// Return true if page of given address is in instruction closed
    /// coupled memory.
    bool isAddrInIccm(size_t addr) const
    { return getAttrib(addr).isIccm(); }

    /// Return true if given address is in memory-mapped register region.
    bool isAddrInMappedRegs(size_t addr) const
    { return getAttrib(addr).isMemMappedReg(); }

    /// Return true if given data address is external to the core.
    bool isDataAddrExternal(size_t addr) const
    {
      PageAttribs attrib = getAttrib(addr);
      return not (attrib.isDccm() or attrib.isMemMappedReg());
    }

    /// Return the simulator memory address corresponding to the
    /// simulated RISCV memory address. This is useful for Linux
    /// emulation.
    bool getSimMemAddr(size_t addr, size_t& simAddr)
    {
      if (addr >= size_)
	return false;
      simAddr = reinterpret_cast<size_t>(data_ + addr);
      return true;
    }

    /// Set the write-access of the page containing the given address
    /// to the given flag. No-op if address is out of bounds.
    void setWriteAccess(size_t addr, bool value)
    {
      size_t ix = getPageIx(addr);
      if (ix >= attribs_.size())
	return;
      attribs_[ix].setWrite(value);
    }

    /// Set the read-access of the page containing the given address
    /// to the given flag. No-op if address is out of bounds.
    void setReadAccess(size_t addr, bool value)
    {
      size_t ix = getPageIx(addr);
      if (ix >= attribs_.size())
	return;
      attribs_[ix].setRead(value);
    }

    /// Set the execute flag of the page containing the given address
    /// to the given flag. No-op if address is out of bounds.
    void setExecAccess(size_t addr, bool value)
    {
      size_t ix = getPageIx(addr);
      if (ix >= attribs_.size())
	return;
      attribs_[ix].setExec(value);
    }

    /// Track LR instructin resrvations.
    struct Reservation
    {
      size_t addr_ = 0;
      unsigned size_ = 0;
      bool valid_ = false;
    };
      
    /// Invalidate LR reservations matching address of poked/written
    /// bytes and belonging to harts other than the given hart-id. The
    /// memory tracks one reservation per hart indexed by local hart
    /// ids.
    void invalidateOtherHartLr(unsigned localHartId, size_t addr,
                               unsigned storeSize)
    {
      for (size_t i = 0; i < reservations_.size(); ++i)
        {
          if (i == localHartId) continue;
          auto& res = reservations_[i];
          if (addr >= res.addr_ and (addr - res.addr_) < res.size_)
            res.valid_ = false;
          else if (addr < res.addr_ and (res.addr_ - addr) < storeSize)
            res.valid_ = false;
        }
    }

    /// Invalidate LR reservation corresponding to the given hart.
    void invalidateLr(unsigned localHartId)
    { reservations_.at(localHartId).valid_ = false; }

    /// Make a LR reservation for the given hart.
    void makeLr(unsigned localHartId, size_t addr, unsigned size)
    {
      auto& res = reservations_.at(localHartId);
      res.addr_ = addr;
      res.size_ = size;
      res.valid_ = true;
    }

    /// Return true if given hart has a valid LR reservation for the
    /// given address.
    bool hasLr(unsigned localHartId, size_t addr) const
    {
      auto& res = reservations_.at(localHartId);
      return res.valid_ and res.addr_ == addr;
    }

  private:

    /// Information about last write operation by a hart.
    struct LastWriteData
    {
      unsigned size_ = 0;
      size_t addr_ = 0;
      uint64_t value_ = 0;
      uint64_t prevValue_ = 0;
    };

    size_t size_;        // Size of memory in bytes.
    uint8_t* data_;      // Pointer to memory data.

    // Memory is organized in regions (e.g. 256 Mb). Each region is
    // organized in pages (e.g 4kb). Each page is associated with
    // access attributes. Memory mapped register pages are also
    // associated with write-masks (one 4-byte mask per word).
    size_t regionCount_    = 16;
    size_t regionSize_     = 256*1024*1024;
    std::vector<bool> regionConfigured_; // One per region.

    size_t pageCount_     = 1024*1024; // Should be derived from page size.
    size_t pageSize_      = 4*1024;    // Must be a power of 2.
    unsigned pageShift_   = 12;        // Shift address by this to get page no.
    unsigned regionShift_ = 28;        // Shift address by this to get region no.
    unsigned regionMask_  = 0xf;       // This should depend on mem size.

    std::mutex amoMutex_;
    std::mutex lrMutex_;

    // Attributes are assigned to pages.
    std::vector<PageAttribs> attribs_;      // One entry per page.
    std::vector<std::vector<uint32_t> > masks_;  // One vector per page.

    std::vector<size_t> mmrPages_;  // Memory mapped register pages.

    bool checkUnmappedElf_ = true;

    std::unordered_map<std::string, ElfSymbol> symbols_;

    std::vector<Reservation> reservations_;
    std::vector<LastWriteData> lastWriteData_;
  };
}
