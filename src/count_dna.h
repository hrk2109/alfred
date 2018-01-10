/*
============================================================================
Alfred: BAM alignment statistics
============================================================================
Copyright (C) 2017,2018 Tobias Rausch

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
============================================================================
Contact: Tobias Rausch (rausch@embl.de)
============================================================================
*/

#ifndef COUNT_H
#define COUNT_H

#include <limits>

#include <boost/icl/split_interval_map.hpp>
#include <boost/dynamic_bitset.hpp>
#include <boost/unordered_map.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/progress.hpp>

#include <htslib/sam.h>

#include "version.h"
#include "util.h"


namespace bamstats
{

  struct CountDNAConfig {
    uint32_t window_size;
    uint32_t window_offset;
    uint32_t window_num;
    uint16_t minQual;
    bool hasIntervalFile;
    std::string sampleName;
    std::vector<bool> validChr;
    boost::filesystem::path bamFile;
    boost::filesystem::path outfile;
    boost::filesystem::path int_file;
  };

  struct ItvChr {
    int32_t start;
    int32_t end;
    std::string id;
  };

  template<typename TConfig>  
  inline bool
  createIntervals(TConfig const& c, std::string const& chr, uint32_t const target_len, std::vector<ItvChr>& intvec) {
    if (c.hasIntervalFile) {
      std::ifstream interval_file(c.int_file.string().c_str(), std::ifstream::in);
      if (interval_file.is_open()) {
	while (interval_file.good()) {
	  std::string intervalLine;
	  getline(interval_file, intervalLine);
	  typedef boost::tokenizer< boost::char_separator<char> > Tokenizer;
	  boost::char_separator<char> sep(" \t,;");
	  Tokenizer tokens(intervalLine, sep);
	  Tokenizer::iterator tokIter = tokens.begin();
	  if (tokIter!=tokens.end()) {
	    std::string chrName=*tokIter++;
	    if (chrName == chr) {
	      if (tokIter!=tokens.end()) {
		ItvChr itv;
		itv.start = boost::lexical_cast<int32_t>(*tokIter++);
		itv.end = boost::lexical_cast<int32_t>(*tokIter++);
		if (itv.start < 0) {
		  std::cerr << "Interval start < 0" << std::endl;
		  return false;
		}
		if (itv.end < 0) {
		  std::cerr << "Interval end < 0" << std::endl;
		  return false;
		}
		if (itv.start >= itv.end) {
		  std::cerr << "Interval start > interval end" << std::endl;
		  return false;
		}
		itv.id = *tokIter;
		intvec.push_back(itv);
	      }
	    }
	  }
	}
	interval_file.close();
      }
    } else {
      // Create artificial intervals
      uint32_t pos = 0;
      unsigned int wSize = c.window_size;
      unsigned int wOffset = c.window_offset;
      if (c.window_num > 0) {
	wSize=(target_len / c.window_num) + 1;
	wOffset=wSize;
      }
      while (pos < target_len) {
	uint32_t window_len = pos+wSize;
	if (window_len > target_len) window_len = target_len;
	ItvChr itv;
	itv.start = pos;
	itv.end = window_len;
	itv.id = chr + ":" + boost::lexical_cast<std::string>(itv.start) + "-" + boost::lexical_cast<std::string>(itv.end);
	intvec.push_back(itv);
	pos += wOffset;
      }
    }
    return true;
  }

  
  template<typename TConfig>
  inline int32_t
  bam_dna_counter(TConfig const& c) {
    
    // Load bam file
    samFile* samfile = sam_open(c.bamFile.string().c_str(), "r");
    hts_idx_t* idx = sam_index_load(samfile, c.bamFile.string().c_str());
    bam_hdr_t* hdr = sam_hdr_read(samfile);

    // Parse BAM file
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "BAM file parsing" << std::endl;
    boost::progress_display show_progress( hdr->n_targets );

    // Pair qualities and features
    typedef boost::unordered_map<std::size_t, uint8_t> TQualities;
    TQualities qualities;

    // Open output file
    boost::iostreams::filtering_ostream dataOut;
    dataOut.push(boost::iostreams::gzip_compressor());
    dataOut.push(boost::iostreams::file_sink(c.outfile.string().c_str(), std::ios_base::out | std::ios_base::binary));
    dataOut << "chr\tstart\tend\tid\t" << c.sampleName << std::endl;
    
    // Iterate chromosomes
    for(int32_t refIndex=0; refIndex < (int32_t) hdr->n_targets; ++refIndex) {
      ++show_progress;

      // Any regions on this chromosome?
      if (!c.validChr[refIndex]) continue;

      // Check we have mapped reads on this chromosome
      bool nodata = true;
      std::string suffix("cram");
      std::string str(c.bamFile.string());
      if ((str.size() >= suffix.size()) && (str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0)) nodata = false;
      uint64_t mapped = 0;
      uint64_t unmapped = 0;
      hts_idx_get_stat(idx, refIndex, &mapped, &unmapped);
      if (mapped) nodata = false;
      if (nodata) continue;

      // Coverage track
      typedef uint16_t TCount;
      uint32_t maxCoverage = std::numeric_limits<TCount>::max();
      typedef std::vector<TCount> TCoverage;
      TCoverage cov(hdr->target_len[refIndex], 0);
      
      // Count reads
      hts_itr_t* iter = sam_itr_queryi(idx, refIndex, 0, hdr->target_len[refIndex]);
      bam1_t* rec = bam_init1();
      int32_t lastAlignedPos = 0;
      std::set<std::size_t> lastAlignedPosReads;
      while (sam_itr_next(samfile, iter, rec) >= 0) {
	if ((rec->core.flag & (BAM_FSECONDARY | BAM_FQCFAIL | BAM_FDUP | BAM_FSUPPLEMENTARY | BAM_FUNMAP | BAM_FMUNMAP)) || (rec->core.tid != rec->core.mtid) || (!(rec->core.flag & BAM_FPAIRED))) continue;
	if (rec->core.qual < c.minQual) continue;

	// Clean-up the read store for identical alignment positions
	if (rec->core.pos > lastAlignedPos) {
	  lastAlignedPosReads.clear();
	  lastAlignedPos = rec->core.pos;
	}
	
	if ((rec->core.pos < rec->core.mpos) || ((rec->core.pos == rec->core.mpos) && (lastAlignedPosReads.find(hash_string(bam_get_qname(rec))) == lastAlignedPosReads.end()))) {
	  // First read
	  lastAlignedPosReads.insert(hash_string(bam_get_qname(rec)));
	  std::size_t hv = hash_pair(rec);
	  qualities[hv] = rec->core.qual;
	} else {
	  // Second read
	  std::size_t hv = hash_pair_mate(rec);
	  if (qualities.find(hv) == qualities.end()) continue; // Mate discarded
	  uint8_t pairQuality = std::min((uint8_t) qualities[hv], (uint8_t) rec->core.qual);
	  qualities[hv] = 0;

	  // Pair quality
	  if (pairQuality < c.minQual) continue; // Low quality pair

	  // Count mid point
	  int32_t midPoint = rec->core.pos + halfAlignmentLength(rec);
	  if ((midPoint < (int32_t) hdr->target_len[refIndex]) && (cov[midPoint] < maxCoverage - 1)) ++cov[midPoint];
	}
      }
      // Clean-up
      bam_destroy1(rec);
      hts_itr_destroy(iter);
      qualities.clear();

      // Assign read counts
      std::vector<ItvChr> itv;
      if (!createIntervals(c, std::string(hdr->target_name[refIndex]), hdr->target_len[refIndex], itv)) {
	std::cerr << "Interval parsing failed!" << std::endl;
	return 1;
      }
      std::sort(itv.begin(), itv.end(), SortIntervalStart<ItvChr>());
      for(uint32_t i = 0; i < itv.size(); ++i) {
	uint64_t covsum = 0;
	for(int32_t k = itv[i].start; k < itv[i].end; ++k) covsum += cov[k];
	dataOut << std::string(hdr->target_name[refIndex]) << "\t" << itv[i].start << "\t" << itv[i].end << "\t" << itv[i].id << "\t" << covsum << std::endl;
      }
    }
	  
    // clean-up
    bam_hdr_destroy(hdr);
    hts_idx_destroy(idx);
    sam_close(samfile);


    
    return 0;
  }

  
  template<typename TConfig>
  inline int32_t
  countDNARun(TConfig const& c) {

#ifdef PROFILE
    ProfilerStart("alfred.prof");
#endif

    int32_t retparse = bam_dna_counter(c);
    if (retparse != 0) {
      std::cerr << "Error in read counting!" << std::endl;
      return 1;
    }

    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] Done." << std::endl;
    
#ifdef PROFILE
    ProfilerStop();
#endif

    return 0;
  }


  int count_dna(int argc, char **argv) {
    CountDNAConfig c;

    // Parameter
    boost::program_options::options_description generic("Generic options");
    generic.add_options()
      ("help,?", "show help message")
      ("map-qual,m", boost::program_options::value<uint16_t>(&c.minQual)->default_value(10), "min. mapping quality")
      ("outfile,o", boost::program_options::value<boost::filesystem::path>(&c.outfile)->default_value("cov.gz"), "coverage output file")
      ;

    boost::program_options::options_description window("Window options");
    window.add_options()
      ("window-size,s", boost::program_options::value<uint32_t>(&c.window_size)->default_value(10000), "window size")
      ("window-offset,t", boost::program_options::value<uint32_t>(&c.window_offset)->default_value(10000), "window offset")
      ("window-num,n", boost::program_options::value<uint32_t>(&c.window_num)->default_value(0), "#windows per chr, used if #n>0")
      ("interval-file,i", boost::program_options::value<boost::filesystem::path>(&c.int_file), "interval file, used if present")
      ;

    boost::program_options::options_description hidden("Hidden options");
    hidden.add_options()
      ("input-file", boost::program_options::value<boost::filesystem::path>(&c.bamFile), "input bam file")
      ;

    boost::program_options::positional_options_description pos_args;
    pos_args.add("input-file", -1);

    // Set the visibility
    boost::program_options::options_description cmdline_options;
    cmdline_options.add(generic).add(window).add(hidden);
    boost::program_options::options_description visible_options;
    visible_options.add(generic).add(window);

    // Parse command-line
    boost::program_options::variables_map vm;
    boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(cmdline_options).positional(pos_args).run(), vm);
    boost::program_options::notify(vm);

    // Check command line arguments
    if ((vm.count("help")) || (!vm.count("input-file"))) {
      printTitle("Alfred");
      std::cout << "Usage: alfred " << argv[0] << " [OPTIONS] -g <ref.fa> <aligned.bam>" << std::endl;
      std::cout << visible_options << "\n";
      return 1;
    }

    // Check bam file
    if (!(boost::filesystem::exists(c.bamFile) && boost::filesystem::is_regular_file(c.bamFile) && boost::filesystem::file_size(c.bamFile))) {
      std::cerr << "Alignment file is missing: " << c.bamFile.string() << std::endl;
      return 1;
    } else {
      samFile* samfile = sam_open(c.bamFile.string().c_str(), "r");
      if (samfile == NULL) {
	std::cerr << "Fail to open file " << c.bamFile.string() << std::endl;
	return 1;
      }
      hts_idx_t* idx = sam_index_load(samfile, c.bamFile.string().c_str());
      if (idx == NULL) {
	if (bam_index_build(c.bamFile.string().c_str(), 0) != 0) {
	  std::cerr << "Fail to open index for " << c.bamFile.string() << std::endl;
	  return 1;
	}
      }
      bam_hdr_t* hdr = sam_hdr_read(samfile);
      if (hdr == NULL) {
	std::cerr << "Fail to open header for " << c.bamFile.string() << std::endl;
	return 1;
      }

      // Get sample name
      std::string sampleName;
      if (!getSMTag(std::string(hdr->text), c.bamFile.stem().string(), sampleName)) {
	std::cerr << "Only one sample (@RG:SM) is allowed per input BAM file " << c.bamFile.string() << std::endl;
	return 1;
      } else c.sampleName = sampleName;

      // Check input intervals (if present)
      if (vm.count("interval-file")) {
	c.validChr.resize(hdr->n_targets, false);
	if (!(boost::filesystem::exists(c.int_file) && boost::filesystem::is_regular_file(c.int_file) && boost::filesystem::file_size(c.int_file))) {
	  std::cerr << "Interval file is missing: " << c.int_file.string() << std::endl;
	  return 1;
	}
	std::string oldChr;
	std::ifstream interval_file(c.int_file.string().c_str(), std::ifstream::in);
	if (interval_file.is_open()) {
	  while (interval_file.good()) {
	    std::string intervalLine;
	    getline(interval_file, intervalLine);
	    typedef boost::tokenizer< boost::char_separator<char> > Tokenizer;
	    boost::char_separator<char> sep(" \t,;");
	    Tokenizer tokens(intervalLine, sep);
	    Tokenizer::iterator tokIter = tokens.begin();
	    if (tokIter!=tokens.end()) {
	      std::string chrName=*tokIter++;
	      if (chrName.compare(oldChr) != 0) {
		oldChr = chrName;
		int32_t tid = bam_name2id(hdr, chrName.c_str());
		if ((tid < 0) || (tid >= (int32_t) hdr->n_targets)) {
		  std::cerr << "Interval file chromosome " << chrName << " is NOT present in your BAM file header " << c.bamFile.string() << std::endl;
		  return 1;
		}
		c.validChr[tid] = true;
	      }
	    }
	  }
	  interval_file.close();
	}
	c.hasIntervalFile= true;
      } else {
	c.validChr.resize(hdr->n_targets, true); // All chromosomes need to be parsed
	c.hasIntervalFile = false;
      }

      // Clean-up
      bam_hdr_destroy(hdr);
      hts_idx_destroy(idx);
      sam_close(samfile);
    }

    // Show cmd
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] ";
    std::cout << "alfred ";
    for(int i=0; i<argc; ++i) { std::cout << argv[i] << ' '; }
    std::cout << std::endl;

    return countDNARun(c);
  }

  
}

#endif