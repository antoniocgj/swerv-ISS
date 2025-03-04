//
// SPDX-License-Identifier: GPL-3.0-or-later
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

#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include "HartConfig.hpp"
#include "Hart.hpp"


using namespace WdRiscv;


HartConfig::HartConfig()
{
  config_ = new nlohmann::json();
}


HartConfig::~HartConfig()
{
  delete config_;
  config_ = nullptr;
}


bool
HartConfig::loadConfigFile(const std::string& filePath)
{
  std::ifstream ifs(filePath);
  if (not ifs.good())
    {
      std::cerr << "Failed to open config file '" << filePath
		<< "' for input.\n";
      return false;
    }

  try
    {
      ifs >> *config_;
    }
  catch (std::exception& e)
    {
      std::cerr << e.what() << "\n";
      return false;
    }
  catch (...)
    {
      std::cerr << "Caught unknown exception while parsing "
		<< " config file '" << filePath << "'\n";
      return false;
    }

  return true;
}


namespace WdRiscv
{
  
  /// Convert given json value to an unsigned integer honoring
  /// hexadecimal prefix (0x) if any.
  template <typename URV>
  URV
  getJsonUnsigned(const std::string& tag, const nlohmann::json& js)
  {
    if (js.is_number())
      return js.get<URV>();
    if (js.is_string())
      {
	char *end = nullptr;
	std::string str = js.get<std::string>();
	uint64_t u64 = strtoull(str.c_str(), &end, 0);
	if (end and *end)
	  std::cerr << "Invalid config file value for '" << tag << "': "
		    << str << '\n';
	URV val = static_cast<URV>(u64);
	if (val != u64)
	  std::cerr << "Overflow in config file value for '" << tag << "': "
		    << str << '\n';
	return val;
      }
    std::cerr << "Config file entry '" << tag << "' must contain a number\n";
    return 0;
  }


  /// Convert given json array value to an vector of unsigned integers
  /// honoring any hexadecimal prefix (0x) if any.
  template <typename URV>
  std::vector<URV>
  getJsonUnsignedVec(const std::string& tag, const nlohmann::json& js)
  {
    std::vector<URV> vec;

    if (not js.is_array())
      {
	std::cerr << "Invalid config file value for '" << tag << "'"
		  << " -- expecting array of numbers\n";
	return vec;
      }

    for (const auto& item :js)
      {
	if (item.is_number())
	  vec.push_back(item.get<unsigned>());
	else if (item.is_string())
	  {
	    char *end = nullptr;
	    std::string str = item.get<std::string>();
	    uint64_t u64 = strtoull(str.c_str(), &end, 0);
	    if (end and *end)
	      {
		std::cerr << "Invalid config file value for '" << tag << "': "
			  << str << '\n';
		continue;
	      }

	    URV val = static_cast<URV>(u64);
	    if (val != u64)
	      std::cerr << "Overflow in config file value for '" << tag << "': "
			  << str << '\n';

	    vec.push_back(val);
	  }
	else
	  std::cerr << "Invalid config file value for '" << tag << "'"
		    << " -- expecting array of number\n";
      }

    return vec;
  }


  /// Convert given json value to a boolean.
  bool
  getJsonBoolean(const std::string& tag, const nlohmann::json& js)
  {
    if (js.is_boolean())
      return js.get<bool>();
    if (js.is_number())
      return js.get<unsigned>();
    if (js.is_string())
      {
	std::string str = js.get<std::string>();
	if (str == "0" or str == "false" or str == "False")
	  return false;
	if (str == "1" or str == "true" or str == "True")
	  return true;
	std::cerr << "Invalid config file value for '" << tag << "': "
		  << str << '\n';
	return false;
      }
    std::cerr << "Config file entry '" << tag << "' must contain a bool\n";
    return false;
  }

}


static
bool
validateStackChecker(const nlohmann::json& csrs)
{
  // If any of the stack checker CSRs is present then all must be
  // present.
  auto tags = { "mspcba", "mspcta", "mspcc" };
  std::string present;
  unsigned count = 0;
  for (const auto& tag : tags)
    if (csrs.count(tag))
      {
	present = tag;
	count++;
      }

  if (count == 0)
    return true;

  if (count != tags.size())
    {
      std::cerr << "Error: Not all stack checker CSRs are defined:\n";
      std::cerr << "  Defined: ";
      std::string sep = "";
      for (const auto& tag: tags)
	if (csrs.count(tag))
	  {
	    std::cerr << sep << tag;
	    sep = ", ";
	  }

      sep.clear();
      std::cerr << "  Missing: ";
      for (const auto& tag: tags)
	if (not csrs.count(tag))
	  {
	    std::cerr << sep << tag;
	    sep = ", ";
	  }

      return false;
    }

  return true;
}


template <typename URV>
static
bool
applyCsrConfig(Hart<URV>& hart, const nlohmann::json& config, bool verbose)
{
  if (not config.count("csr"))
    return true;  // Nothing to apply

  const auto& csrs = config.at("csr");
  if (not csrs.is_object())
    {
      std::cerr << "Invalid csr entry in config file (expecting an object)\n";
      return false;
    }

  unsigned errors = 0;
  for (auto it = csrs.begin(); it != csrs.end(); ++it)
    {
      const std::string& csrName = it.key();
      const auto& conf = it.value();

      URV reset = 0, mask = 0, pokeMask = 0;
      bool isDebug = false, exists = true, shared = false;

      Csr<URV>* csr = hart.findCsr(csrName);
      if (csr)
	{
	  reset = csr->getResetValue();
	  mask = csr->getWriteMask();
	  pokeMask = csr->getPokeMask();
	  isDebug = csr->isDebug();
	}

      if (conf.count("reset"))
	reset = getJsonUnsigned<URV>(csrName + ".reset", conf.at("reset"));

      if (conf.count("mask"))
	{
	  mask = getJsonUnsigned<URV>(csrName + ".mask", conf.at("mask"));

	  // If defining a non-standard CSR (as popposed to
	  // configuring an existing CSR) then default the poke-mask
	  // to the write-mask.
	  if (not csr)
	    pokeMask = mask;
	}

      if (conf.count("poke_mask"))
	pokeMask = getJsonUnsigned<URV>(csrName + ".poke_mask",
				   conf.at("poke_mask"));

      if (conf.count("debug"))
	isDebug = getJsonBoolean(csrName + ".bool", conf.at("debug"));

      if (conf.count("exists"))
	exists = getJsonBoolean(csrName + ".bool", conf.at("exists"));

      if (conf.count("shared"))
        shared = getJsonBoolean(csrName + ".bool", conf.at("shared"));

      // If number present and csr is not defined, then define a new
      // CSR; otherwise, configure.
      if (conf.count("number"))
	{
	  unsigned number = getJsonUnsigned<unsigned>(csrName + ".number",
						      conf.at("number"));
	  if (csr)
	    {
	      if (csr->getNumber() != CsrNumber(number))
		{
		  std::cerr << "Invalid config file entry for CSR "
			    << csrName << ": Number (0x" << std::hex << number
			    << ") does not match that of previous definition ("
			    << "0x" << unsigned(csr->getNumber())
			    << ")\n" << std::dec;
		  errors++;
		  continue;
		}
	      // If number matches we configure below
	    }
	  else if (hart.defineCsr(csrName, CsrNumber(number), exists,
				  reset, mask, pokeMask, isDebug))
	    {
	      csr = hart.findCsr(csrName);
	      assert(csr);
	    }
	  else
	    {
	      std::cerr << "Invalid config file CSR definition with name "
			<< csrName << " and number 0x" << std::hex << number
			<< ": Number already in use\n" << std::dec;
	      errors++;
	      continue;
	    }
	}

      bool exists0 = csr->isImplemented(), isDebug0 = csr->isDebug();
      bool shared0 = csr->isShared();
      URV reset0 = csr->getResetValue(), mask0 = csr->getWriteMask();
      URV pokeMask0 = csr->getPokeMask();

      if (csrName == "mhartstart")
        if (hart.localHartId() == 0 and (reset & 1) == 0)
          std::cerr << "Warning: Bit corresponding to hart 0 is cleared "
                    << "in reset value of mhartstart CSR -- Bit is ignored\n";

      if (not hart.configCsr(csrName, exists, reset, mask, pokeMask,
			     isDebug, shared))
	{
	  std::cerr << "Invalid CSR (" << csrName << ") in config file.\n";
	  errors++;
	}
      else if (verbose)
	{
	  if (exists0 != exists or isDebug0 != isDebug or reset0 != reset or
	      mask0 != mask or pokeMask0 != pokeMask)
	    {
	      std::cerr << "Configuration of CSR (" << csrName <<
		") changed in config file:\n";

	      if (exists0 != exists)
		std::cerr << "  implemented: " << exists0 << " to "
			  << exists << '\n';

	      if (isDebug0 != isDebug)
		std::cerr << "  debug: " << isDebug0 << " to "
			  << isDebug << '\n';

	      if (shared0 != shared)
		std::cerr << "  shred: " << shared0 << " to "
			  << shared << '\n';

	      if (reset0 != reset)
		std::cerr << "  reset: 0x" << std::hex << reset0
			  << " to 0x" << reset << '\n' << std::dec;

	      if (mask0 != mask)
		std::cerr << "  mask: 0x" << std::hex << mask0
			  << " to 0x" << mask << '\n' << std::dec;

	      if (pokeMask0 != pokeMask)
		std::cerr << "  poke_mask: " << std::hex << pokeMask0
			  << " to 0x" << pokeMask << '\n' << std::dec;
	    }
	}
    }

  // Stack checker.
  if (not validateStackChecker(csrs))
    errors++;

  return errors == 0;
}


template <typename URV>
static
bool
applyPicConfig(Hart<URV>& hart, const nlohmann::json& config)
{
  if (not config.count("pic"))
    return true;  // Nothing to apply.

  const auto& pic = config.at("pic");
  for (const auto& tag : { "region", "size", "offset", "mpiccfg_offset",
	"meipl_offset", "meip_offset", "meie_offset", "meigwctrl_offset",
	"meigwclr_offset", "total_int", "int_words" } )
    {
      if (not pic.count(tag))
	{
	  std::cerr << "Missing '" << tag << "' entry in "
		    << "config file PIC section\n";
	}
    }

  // Define pic region.
  uint64_t region = getJsonUnsigned<URV>("region", pic.at("region"));
  uint64_t size = getJsonUnsigned<URV>("size", pic.at("size"));
  uint64_t regionOffset = getJsonUnsigned<URV>("offset", pic.at("offset"));
  if (not hart.defineMemoryMappedRegisterRegion(region, regionOffset, size))
    return false;

  // Define the memory mapped registers.
  uint64_t smax = getJsonUnsigned<URV>("pic.total_int", pic.at("total_int"));
  uint64_t xmax = getJsonUnsigned<URV>("pic.int_words", pic.at("int_words"));

  unsigned errors = 0;

  // Start by giving all registers in region a mask of zero.
  size_t possibleRegCount = size / 4;
  for (size_t ix = 0; ix < possibleRegCount; ++ix)
    hart.defineMemoryMappedRegisterWriteMask(region, regionOffset, 0, ix, 0);

  std::vector<std::string> names = { "mpiccfg_offset", "meipl_offset",
				     "meip_offset", "meie_offset",
				     "meigwctrl_offset", "meigwclr_offset" };

  // These should be in the config file. The mask for meigwclr is zero
  // because the state is always zero.
  std::vector<uint32_t> masks = { 1, 0xf, 0, 1, 3, 0 };
  std::vector<size_t> counts = { 1, smax, xmax, smax, smax, smax };

  // meipl, meie, meigwctrl and meigwclr indexing start at 1 (instead
  // of 0): adjust
  std::vector<size_t> adjust = { 0, 4, 0, 4, 4, 4 };

  for (size_t i = 0; i < names.size(); ++i)
    {
      auto mask = masks.at(i);
      const auto& name = names.at(i);
      auto count = counts.at(i);

      if (not pic.count(name))
	continue;  // Should be an error.

      uint64_t registerOffset = getJsonUnsigned<URV>(("pic." + name),
						     pic.at(name));
      registerOffset += adjust.at(i);
      for (size_t regIx = 0; regIx < count; ++regIx)
	if (not hart.defineMemoryMappedRegisterWriteMask(region, regionOffset,
							 registerOffset, regIx,
							 mask))
	  errors++;
    }

  return errors == 0;
}


template <typename URV>
static
bool
applyIccmConfig(Hart<URV>& hart, const nlohmann::json& config)
{
  if (not config.count("iccm"))
    return true;

  const auto& iccm = config.at("iccm");
  if (iccm.count("region") and iccm.count("size") and iccm.count("offset"))
    {
      size_t region = getJsonUnsigned<URV>("iccm.region", iccm.at("region"));
      size_t size   = getJsonUnsigned<URV>("iccm.size",   iccm.at("size"));
      size_t offset = getJsonUnsigned<URV>("iccm.offset", iccm.at("offset"));
      return hart.defineIccm(region, offset, size);
    }

  std::cerr << "The ICCM entry in the configuration file must contain "
            << "a region, offset and a size entry.\n";
  return false;
}


template <typename URV>
static
bool
applyDccmConfig(Hart<URV>& hart, const nlohmann::json& config)
{
  if (not config.count("dccm"))
    return true;

  const auto& dccm = config.at("dccm");
  if (dccm.count("region") and dccm.count("size") and dccm.count("offset"))
    {
      size_t region = getJsonUnsigned<URV>("dccm.region", dccm.at("region"));
      size_t size   = getJsonUnsigned<URV>("dccm.size",   dccm.at("size"));
      size_t offset = getJsonUnsigned<URV>("dccm.offset", dccm.at("offset"));
      return hart.defineDccm(region, offset, size);
    }

  std::cerr << "The DCCM entry in the configuration file must contain "
            << "a region, offset and a size entry.\n";
  return false;
}


template <typename URV>
static
bool
applyTriggerConfig(Hart<URV>& hart, const nlohmann::json& config)
{
  if (not config.count("triggers"))
    return true;  // Nothing to apply

  const auto& triggers = config.at("triggers");
  if (not triggers.is_array())
    {
      std::cerr << "Invalid triggers entry in config file (expecting an array)\n";
      return false;
    }

  unsigned errors = 0;
  unsigned ix = 0;
  for (auto it = triggers.begin(); it != triggers.end(); ++it, ++ix)
    {
      const auto& trig = *it;
      std::string name = std::string("trigger") + std::to_string(ix);
      if (not trig.is_object())
	{
	  std::cerr << "Invalid trigger in config file triggers array "
		    << "(expecting an object at index " << ix << ")\n";
	  ++errors;
	  break;
	}
      bool ok = true;
      for (const auto& tag : {"reset", "mask", "poke_mask"})
	if (not trig.count(tag))
	  {
	    std::cerr << "Trigger " << name << " has no '" << tag
		      << "' entry in config file\n";
	    ok = false;
	  }
      if (not ok)
	{
	  errors++;
	  continue;
	}
      auto resets = getJsonUnsignedVec<URV>(name + ".reset", trig.at("reset"));
      auto masks = getJsonUnsignedVec<URV>(name + ".mask", trig.at("mask"));
      auto pokeMasks = getJsonUnsignedVec<URV>(name + ".poke_mask",
					       trig.at("poke_mask"));

      if (resets.size() != 3)
	{
	  std::cerr << "Trigger " << name << ": Bad item count (" << resets.size()
		    << ") for 'reset' field in config file. Expecting 3.\n";
	  ok = false;
	}

      if (masks.size() != 3)
	{
	  std::cerr << "Trigger " << name << ": Bad item count (" << masks.size()
		    << ") for 'mask' field in config file. Expecting 3.\n";
	  ok = false;
	}

      if (pokeMasks.size() != 3)
	{
	  std::cerr << "Trigger " << name << ": Bad item count (" << pokeMasks.size()
		    << ") for 'poke_mask' field in config file. Expecting 3.\n";
	  ok = false;
	}

      if (not ok)
	{
	  errors++;
	  continue;
	}
      if (not hart.configTrigger(ix, resets.at(0), resets.at(1), resets.at(2),
				 masks.at(0), masks.at(1), masks.at(2),
				 pokeMasks.at(0), pokeMasks.at(1),
                                 pokeMasks.at(2)))
	{
	  std::cerr << "Failed to configure trigger " << std::dec << ix << '\n';
	  ++errors;
	}
    }

  return errors == 0;
}


template <typename URV>
static
bool
applyInstMemConfig(Hart<URV>& hart, const nlohmann::json& config)
{
  using std::cerr;

  if (not config.is_array())
    {
      cerr << "Invalid inst entry in config file memmap (execpting an array)\n";
      return false;
    }

  // Pairs of addresses in which inst fetch is allowed.
  std::vector<std::pair<URV,URV>> windows;

  unsigned errors = 0;
  unsigned ix = 0;
  for (auto it = config.begin(); it != config.end(); ++it, ++ix)
    {
      const auto& addrPair = *it;
      if (not addrPair.is_array() or addrPair.size() != 2)
	{
	  cerr << "Invalid address range in config file memap inst ("
	       << "expecting an array of 2 numbers at index " << ix << ")\n";
	  ++errors;
	  break;
	}

      auto vec = getJsonUnsignedVec<URV>("memmap.inst", addrPair);
      if (vec.size() != 2)
	errors++;
      else
	{
	  auto p = std::make_pair(vec.at(0), vec.at(1));
	  windows.push_back(p);
	}
    }

  if (not errors and not windows.empty())
    if (not hart.configMemoryFetch(windows))
      errors++;

  return errors == 0;
}


template <typename URV>
static
bool
applyDataMemConfig(Hart<URV>& hart, const nlohmann::json& config)
{
  using std::cerr;

  if (not config.is_array())
    {
      cerr << "Invalid data entry in config file memmap (execpting an array)\n";
      return false;
    }

  // Pairs of addresses in which data access is allowed.
  std::vector<std::pair<URV,URV>> windows;

  unsigned errors = 0;
  unsigned ix = 0;
  for (auto it = config.begin(); it != config.end(); ++it, ++ix)
    {
      const auto& addrPair = *it;
      if (not addrPair.is_array() or addrPair.size() != 2)
	{
	  cerr << "Invalid address range in config file memap data ("
	       << "expecting an array of 2 numbers at index " << ix << ")\n";
	  ++errors;
	  break;
	}

      auto vec = getJsonUnsignedVec<URV>("memmap.data", addrPair);
      if (vec.size() != 2)
	errors++;
      else
	{
	  auto p = std::make_pair(vec.at(0), vec.at(1));
	  windows.push_back(p);
	}
    }

  if (not errors and not windows.empty())
    if (not hart.configMemoryDataAccess(windows))
      errors++;

  return errors == 0;
}


template<typename URV>
bool
HartConfig::applyMemoryConfig(Hart<URV>& hart, bool /*verbose*/) const
{
  unsigned errors = 0;
  if (not applyIccmConfig(hart, *config_))
    errors++;

  if (not applyDccmConfig(hart, *config_))
    errors++;

  if (not applyPicConfig(hart, *config_))
    errors++;

  hart.finishCcmConfig();

  if (config_ -> count("memmap"))
    {
      // Apply memory protection windows.
      const auto& memmap = config_ -> at("memmap");
      std::string tag = "inst";
      if (memmap.count(tag))
	if (not applyInstMemConfig(hart, memmap.at(tag)))
	  errors++;
      tag = "data";
      if (memmap.count(tag))
	if (not applyDataMemConfig(hart, memmap.at(tag)))
	  errors++;
    }

  return errors == 0;
}


template<typename URV>
bool
HartConfig::applyConfig(Hart<URV>& hart, bool verbose) const
{
  // Define PC value after reset.
  std::string tag = "reset_vec";
  if (config_ -> count(tag))
    {
      URV resetPc = getJsonUnsigned<URV>(tag, config_ -> at(tag));
      hart.defineResetPc(resetPc);
    }

  // Define non-maskable-interrupt pc
  tag = "nmi_vec";
  if (config_ -> count(tag))
    {
      URV nmiPc = getJsonUnsigned<URV>(tag, config_ -> at(tag));
      hart.defineNmiPc(nmiPc);
    }

  // Use ABI register names (e.g. sp instead of x2).
  tag = "abi_names";
  if (config_ -> count(tag))
    {
      bool abiNames = getJsonBoolean(tag, config_ ->at(tag));
      hart.enableAbiNames(abiNames);
    }

  // Atomic instructions illegal outside of DCCM.
  tag = "amo_illegal_outside_dccm";
  if (config_ -> count(tag))
    {
      bool flag = getJsonBoolean(tag, config_ ->at(tag));
      hart.setAmoIllegalOutsideDccm(flag);
    }

  // Ld/st instructions trigger misaligned exception if base address
  // (value in rs1) and effective address refer to regions of
  // different types.
  tag = "effective_address_compatible_with_base";
  if (config_ -> count(tag))
    {
      bool flag = getJsonBoolean(tag, config_ ->at(tag));
      hart.setEaCompatibleWithBase(flag);
    }

  // Enable debug triggers.
  tag ="enable_triggers";
  if (config_ -> count(tag))
    {
      bool et = getJsonBoolean(tag, config_ ->at(tag));
      hart.enableTriggers(et);
    }

  // Enable performance counters.
  tag ="enable_performance_counters";
  if (config_ -> count(tag))
    {
      bool epc = getJsonBoolean(tag, config_ ->at(tag));
      hart.enablePerformanceCounters(epc);
    }

  // Enable rollback of memory on store error.
  tag = "store_error_rollback";
  if (config_ -> count(tag))
    {
      bool ser = getJsonBoolean(tag, config_ -> at(tag));
      hart.enableStoreErrorRollback(ser);
    }

  // Enable rollback of register on load error.
  tag = "load_error_rollback";
  if (config_ -> count(tag))
    {
      bool ler = getJsonBoolean(tag, config_ -> at(tag));
      hart.enableLoadErrorRollback(ler);
    }

  // Enable fast interrupts.
  tag = "fast_interrupt_redirect";
  if (config_ -> count(tag))
    {
      bool flag = getJsonBoolean(tag, config_ -> at(tag));
      hart.enableFastInterrupts(flag);
    }

  // Enable zbb.
  tag = "enable_zbmini";
  if (config_ -> count(tag))
    {
      std::cerr << "Config file tag \"enable_zbmini\" deprecated: "
		<< "Using \"enable_zbb\" and \"enable_zbs\"\n";
      bool flag = getJsonBoolean(tag, config_ -> at(tag));
      hart.enableRvzbb(flag);
      hart.enableRvzbs(flag);
    }

  tag = "enable_zbb";
  if (config_ -> count(tag))
    {
      bool flag = getJsonBoolean(tag, config_ -> at(tag));
      hart.enableRvzbb(flag);
    }

  tag = "enable_zbs";
  if (config_ -> count(tag))
    {
      bool flag = getJsonBoolean(tag, config_ -> at(tag));
      hart.enableRvzbs(flag);
    }

  tag = "load_queue_size";
  if (config_ -> count(tag))
    {
      unsigned lqs = getJsonUnsigned<unsigned>(tag, config_ -> at(tag));
      if (lqs > 64)
	{
	  std::cerr << "Config file load queue size (" << lqs << ") too large"
		    << " -- using 64.\n";
	  lqs = 64;
	}
      hart.setLoadQueueSize(lqs);
    }

  tag = "even_odd_trigger_chains";
  if (config_ -> count(tag))
    {
      bool chainPairs = getJsonBoolean(tag, config_ -> at(tag));
      hart.configEvenOddTriggerChaining(chainPairs);
    }

  unsigned errors = 0;

  tag = "num_mmode_perf_regs";
  if (config_ -> count(tag))
    {
      unsigned count = getJsonUnsigned<unsigned>(tag, config_ -> at(tag));
      if (not hart.configMachineModePerfCounters(count))
	errors++;
    }

  tag = "max_mmode_perf_event";
  if (config_ -> count(tag))
    {
      unsigned maxId = getJsonUnsigned<unsigned>(tag, config_ -> at(tag));
      hart.configMachineModeMaxPerfEvent(maxId);
    }

  if (not applyCsrConfig(hart, *config_, verbose))
    errors++;

  if (not applyTriggerConfig(hart, *config_))
    errors++;

  if (config_ -> count("memmap"))
    {
      const auto& memmap = config_ -> at("memmap");
      tag = "consoleio";
      if (memmap.count(tag))
	{
	  URV io = getJsonUnsigned<URV>("memmap.consoleio", memmap.at(tag));
	  hart.setConsoleIo(io);
	}
    }

  return errors == 0;
}


bool
HartConfig::getXlen(unsigned& xlen) const
{
  if (config_ -> count("xlen"))
    {
      xlen = getJsonUnsigned<uint32_t>("xlen", config_ -> at("xlen"));
      return true;
    }
  return false;
}


bool
HartConfig::getPageSize(size_t& pageSize) const
{
  if (not config_ -> count("memmap"))
    return false;

  auto& mem = config_ -> at("memmap");
  if (not mem.count("page_size"))
    return false;

  pageSize = getJsonUnsigned<size_t>("memmap.page_size", mem.at("page_size"));
  return true;
}


bool
HartConfig::getMemorySize(size_t& memSize) const
{
  if (not config_ -> count("memmap"))
    return false;

  auto& mem = config_ -> at("memmap");
  if (not mem.count("size"))
    return false;

  memSize = getJsonUnsigned<size_t>("memmap.size", mem.at("size"));
  return true;
}


void
HartConfig::clear()
{
  config_ -> clear();
}


/// Associate callbacks with write/poke of mhartstart to start harts
/// when corresponding bits are set in that CSR.
template <typename URV>
void
defineMhartstartSideEffects(std::vector<Hart<URV>*>& harts)
{
  for (auto hart : harts)
    {
      auto csrPtr = hart->findCsr("mhartstart");
      if (not csrPtr)
        continue;
      auto csrNum = csrPtr->getNumber();

      auto post = [&harts] (Csr<URV>&, URV val) -> void {
                    // Start harts corresponding to set bits
                    for (auto ht : harts)
                      {
                        URV id = ht->localHartId();
                        if (val & (URV(1) << id))
                          ht->setStarted(true);
                      }
                  };

      auto pre = [&harts, csrNum] (Csr<URV>&, URV& val) -> void {
                   // Implement write one semantics.
                   URV prev = 0;
                   harts.at(0)->peekCsr(csrNum, prev);
                   val |= prev;
                 };

      csrPtr->registerPostPoke(post);
      csrPtr->registerPostWrite(post);
      csrPtr->registerPrePoke(pre);
      csrPtr->registerPreWrite(pre);
    }
}


/// Associate callback with write/poke of mnmipdel to deletage
/// non-maskable-interrupts to harts.
template <typename URV>
void
defineMnmipdelSideEffects(std::vector<Hart<URV>*>& harts)
{
  for (auto hart : harts)
    {
      auto csrPtr = hart->findCsr("mnmipdel");
      if (not csrPtr)
        continue;

      // Enable NMI for harts corresponding to set bits in mnmipdel.
      auto post = [&harts] (Csr<URV>& csr, URV val) -> void {
                    if ((val & csr.getWriteMask()) == 0)
                      return;
                    for (auto ht : harts)
                      {
                        URV id = ht->localHartId();
                        bool enable = (val & (URV(1) << id)) != 0;
                        ht->enableNmi(enable);
                      }
                  };

      // If an attempt to change writeable bits to all-zero, keep
      // previous value.
      auto pre = [&harts] (Csr<URV>& csr, URV& val) -> void {
                   URV prev = csr.read();
                   if ((val & csr.getWriteMask()) == 0)
                     val = prev;
                 };

      // On reset, enable NMI in harts according to the bits of mnmipdel
      auto reset = [hart] (Csr<URV>& csr) -> void {
                   URV val = csr.read();
                   URV id = hart->localHartId();
                   bool flag = (val & (URV(1) << id)) != 0;
                   hart->enableNmi(flag);
                 };

      csrPtr->registerPostPoke(post);
      csrPtr->registerPostWrite(post);

      csrPtr->registerPrePoke(pre);
      csrPtr->registerPreWrite(pre);

      csrPtr->registerPostReset(reset);
    }
}


/// Associate callback with write/poke of mpmpc
template <typename URV>
void
defineMpmcSideEffects(std::vector<Hart<URV>*>& harts)
{
  for (auto hart : harts)
    {
      auto csrPtr = hart->findCsr("mpmc");
      if (not csrPtr)
        continue;

      // Writing 3 to pmpc enables external interrupts unless in debug mode.
      auto prePoke = [hart] (Csr<URV>& csr, URV& val) -> void {
                       if (hart->inDebugMode() or (val & 3) != 3 or
                           (val & csr.getPokeMask()) == 0)
                         return;
                       URV mval = 0;
                       if (not hart->peekCsr(CsrNumber::MSTATUS, mval))
                         return;
                       MstatusFields<URV> fields(mval);
                       fields.bits_.MIE = 1;
                       hart->pokeCsr(CsrNumber::MSTATUS, fields.value_);
                     };

      auto preWrite = [hart] (Csr<URV>& csr, URV& val) -> void {
                        if (hart->inDebugMode() or (val & 3) != 3 or
                           (val & csr.getWriteMask()) == 0)
                          return;
                        URV mval = 0;
                        if (not hart->peekCsr(CsrNumber::MSTATUS, mval))
                          return;
                       MstatusFields<URV> fields(mval);
                       fields.bits_.MIE = 1;
                       hart->pokeCsr(CsrNumber::MSTATUS, fields.value_);
                       hart->recordCsrWrite(CsrNumber::MSTATUS);
                     };


      csrPtr->registerPrePoke(prePoke);
      csrPtr->registerPreWrite(preWrite);
    }
}


template <typename URV>
bool
HartConfig::finalizeCsrConfig(std::vector<Hart<URV>*>& harts) const
{
  if (harts.empty())
    return false;

  // Make shared CSRs in each hart with hart-id greater than zero
  // point to the corresponding values in hart zero.
  auto hart0 = harts.at(0);
  assert(hart0);
  for (auto hart : harts)
    if (hart != hart0)
      hart->tieSharedCsrsTo(*hart0);

  // The following are WD non-standard CSRs. We implement their
  // actions by associating callbacks the write/poke CSR methods.
  defineMhartstartSideEffects(harts);
  defineMnmipdelSideEffects(harts);

#if 0
  // Unfortuntately, this breaks g++7.1 and g++9.1
  defineMpmcSideEffects(harts);
#else
  // Associate callback with write/poke of mpmpc
  for (auto hart : harts)
    {
      auto csrPtr = hart->findCsr("mpmc");
      if (not csrPtr)
        continue;

      // Writing 3 to pmpc enables external interrupts unless in debug mode.
      auto prePoke = [hart] (Csr<URV>& csr, URV& val) -> void {
                       if (hart->inDebugMode() or (val & 3) != 3 or
                           (val & csr.getPokeMask()) == 0)
                         return;
                       URV mval = 0;
                       if (not hart->peekCsr(CsrNumber::MSTATUS, mval))
                         return;
                       MstatusFields<URV> fields(mval);
                       fields.bits_.MIE = 1;
                       hart->pokeCsr(CsrNumber::MSTATUS, fields.value_);
                     };

      auto preWrite = [hart] (Csr<URV>& csr, URV& val) -> void {
                        if (hart->inDebugMode() or (val & 3) != 3 or
                           (val & csr.getWriteMask()) == 0)
                          return;
                        URV mval = 0;
                        if (not hart->peekCsr(CsrNumber::MSTATUS, mval))
                          return;
                       MstatusFields<URV> fields(mval);
                       fields.bits_.MIE = 1;
                       hart->pokeCsr(CsrNumber::MSTATUS, fields.value_);
                       hart->recordCsrWrite(CsrNumber::MSTATUS);
                     };


      csrPtr->registerPrePoke(prePoke);
      csrPtr->registerPreWrite(preWrite);
    }
#endif

  return true;
}


// Instantiate tempate member functions

template bool HartConfig::applyConfig<uint32_t>(Hart<uint32_t>&, bool) const;
template bool HartConfig::applyConfig<uint64_t>(Hart<uint64_t>&, bool) const;

template bool HartConfig::applyMemoryConfig<uint32_t>(Hart<uint32_t>&, bool) const;
template bool HartConfig::applyMemoryConfig<uint64_t>(Hart<uint64_t>&, bool) const;

template bool HartConfig::finalizeCsrConfig<uint32_t>(std::vector<Hart<uint32_t>*>&) const;
template bool HartConfig::finalizeCsrConfig<uint64_t>(std::vector<Hart<uint64_t>*>&) const;
