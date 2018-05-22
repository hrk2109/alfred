/*
============================================================================
Alfred: BAM alignment statistics
============================================================================
Copyright (C) 2017-2018 Tobias Rausch

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

#ifndef COUNT_JUNCTION_H
#define COUNT_JUNCTION_H

#include <limits>

#include <boost/icl/split_interval_map.hpp>
#include <boost/dynamic_bitset.hpp>
#include <boost/unordered_map.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/progress.hpp>

#include <htslib/sam.h>
#include <htslib/faidx.h>

#include "version.h"
#include "util.h"
#include "gtf.h"
#include "gff3.h"
#include "bed.h"


namespace bamstats
{

  struct CountJunctionConfig {
    typedef std::map<std::string, int32_t> TChrMap;
    
    unsigned short minQual;
    uint8_t inputFileFormat;   // 0 = gtf, 1 = bed, 2 = gff3
    TChrMap nchr;
    std::string sampleName;
    std::string idname;
    std::string feature;
    boost::filesystem::path gtfFile;
    boost::filesystem::path bedFile;
    boost::filesystem::path bamFile;
    boost::filesystem::path outintra;
    boost::filesystem::path outinter;
  };


  template<typename TConfig, typename TGenomicRegions, typename TGenomicExonJunction>
  inline int32_t
  countExonJct(TConfig const& c, TGenomicRegions& gRegions, TGenomicExonJunction& ejct) {
    typedef typename TGenomicRegions::value_type TChromosomeRegions;
    typedef typename TGenomicExonJunction::value_type TExonJctMap;
    
    // Load bam file
    samFile* samfile = sam_open(c.bamFile.string().c_str(), "r");
    hts_idx_t* idx = sam_index_load(samfile, c.bamFile.string().c_str());
    bam_hdr_t* hdr = sam_hdr_read(samfile);

    // Parse BAM file
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "BAM file parsing" << std::endl;
    boost::progress_display show_progress( hdr->n_targets );

    // Iterate chromosomes
    for(int32_t refIndex=0; refIndex < (int32_t) hdr->n_targets; ++refIndex) {
      ++show_progress;
      if (gRegions[refIndex].empty()) continue;

      // Sort by position
      std::sort(gRegions[refIndex].begin(), gRegions[refIndex].end(), SortIntervalStart<IntervalLabelId>());
      int32_t maxExonLength = 0;
      for(uint32_t i = 0; i < gRegions[refIndex].size(); ++i) {
	if ((gRegions[refIndex][i].end - gRegions[refIndex][i].start) > maxExonLength) {
	  maxExonLength = gRegions[refIndex][i].end - gRegions[refIndex][i].start;
	}
      }

      // Flag junction positions
      typedef boost::dynamic_bitset<> TBitSet;
      TBitSet featureBitMap(hdr->target_len[refIndex]);
      for(uint32_t i = 0; i < gRegions[refIndex].size(); ++i) {
	featureBitMap[gRegions[refIndex][i].start] = 1;
	featureBitMap[gRegions[refIndex][i].end] = 1;
      }

      // Count reads
      hts_itr_t* iter = sam_itr_queryi(idx, refIndex, 0, hdr->target_len[refIndex]);
      bam1_t* rec = bam_init1();
      while (sam_itr_next(samfile, iter, rec) >= 0) {
	if (rec->core.flag & (BAM_FQCFAIL | BAM_FDUP | BAM_FUNMAP)) continue;
	if (rec->core.qual < c.minQual) continue; // Low quality read

	// Get read sequence
	std::string sequence;
	sequence.resize(rec->core.l_qseq);
	uint8_t* seqptr = bam_get_seq(rec);
	for (int32_t i = 0; i < rec->core.l_qseq; ++i) sequence[i] = "=ACMGRSVTWYHKDBN"[bam_seqi(seqptr, i)];

	// Parse CIGAR
	uint32_t* cigar = bam_get_cigar(rec);
	int32_t gp = rec->core.pos; // Genomic position
	int32_t sp = 0; // Sequence position
	for (std::size_t i = 0; i < rec->core.n_cigar; ++i) {
	  if (bam_cigar_op(cigar[i]) == BAM_CSOFT_CLIP) {
	    sp += bam_cigar_oplen(cigar[i]);
	    //if (featureBitMap[gp]) featurepos.push_back(gp);
	  }
	  else if (bam_cigar_op(cigar[i]) == BAM_CINS) sp += bam_cigar_oplen(cigar[i]);
	  else if (bam_cigar_op(cigar[i]) == BAM_CDEL) gp += bam_cigar_oplen(cigar[i]);
	  else if (bam_cigar_op(cigar[i]) == BAM_CREF_SKIP) {
	    int32_t gpStart = gp;
	    gp += bam_cigar_oplen(cigar[i]);
	    int32_t gpEnd = gp;
	    if ((featureBitMap[gpStart]) && (featureBitMap[gpEnd])) {
	      typename TChromosomeRegions::const_iterator vIt = std::lower_bound(gRegions[refIndex].begin(), gRegions[refIndex].end(), IntervalLabelId(std::max(0, gpStart - maxExonLength)), SortIntervalStart<IntervalLabelId>());
	      for(; vIt != gRegions[refIndex].end(); ++vIt) {
		if (vIt->end < gpStart) continue;
		if (vIt->start > gpStart) break; // Sorted intervals so we can stop searching
		if (vIt->end == gpStart) {
		  // Find junction partner
		  typename TChromosomeRegions::const_iterator vItNext = vIt;
		  ++vItNext;
		  for(; vItNext != gRegions[refIndex].end(); ++vItNext) {
		    if (vItNext->end < gpEnd) continue;
		    if (vItNext->start > gpEnd) break; // Sorted intervals so we can stop searching
		    if (vItNext->start == gpEnd) {
		      if (vIt->eid < vItNext->eid) {
			typename TExonJctMap::iterator itEjct = ejct[refIndex].find(std::make_pair(vIt->eid, vItNext->eid));
			if (itEjct != ejct[refIndex].end()) ++itEjct->second;
			else ejct[refIndex].insert(std::make_pair(std::make_pair(vIt->eid, vItNext->eid), 1));
		      }
		    }
		  }
		}
	      }
	    }
	  }
	  else if (bam_cigar_op(cigar[i]) == BAM_CHARD_CLIP) {
	    //Nop
	  } else if (bam_cigar_op(cigar[i]) == BAM_CMATCH) {
	    sp += bam_cigar_oplen(cigar[i]);
	    gp += bam_cigar_oplen(cigar[i]);
	  } else {
	    std::cerr << "Unknown Cigar options" << std::endl;
	    return 1;
	  }
	}
      }
      // Clean-up
      bam_destroy1(rec);
      hts_itr_destroy(iter);
    }

    // clean-up
    bam_hdr_destroy(hdr);
    hts_idx_destroy(idx);
    sam_close(samfile);
    return 0;
  }
  
  template<typename TConfig>
  inline int32_t
  countJunctionRun(TConfig const& c) {

#ifdef PROFILE
    ProfilerStart("alfred.prof");
#endif

    // Parse GTF file
    typedef std::vector<IntervalLabelId> TChromosomeRegions;
    typedef std::vector<TChromosomeRegions> TGenomicRegions;
    TGenomicRegions gRegions(c.nchr.size(), TChromosomeRegions());
    typedef std::vector<std::string> TGeneIds;
    TGeneIds geneIds;
    int32_t tf = 0;
    if (c.inputFileFormat == 0) tf = parseGTFAll(c, gRegions, geneIds);
    else if (c.inputFileFormat == 1) tf = parseBEDAll(c, gRegions, geneIds);
    else if (c.inputFileFormat == 2) tf = parseGFF3All(c, gRegions, geneIds);
    if (tf == 0) {
      std::cerr << "Error parsing GTF/GFF3/BED file!" << std::endl;
      return 1;
    }

    // Exon junction counting
    typedef std::pair<int32_t, int32_t> TExonPair;
    typedef std::map<TExonPair, uint32_t> TExonJctCount;
    typedef std::vector<TExonJctCount> TGenomicExonJctCount;
    TGenomicExonJctCount ejct(c.nchr.size(), TExonJctCount());
    int32_t retparse = countExonJct(c, gRegions, ejct);
    if (retparse != 0) {
      std::cerr << "Error exon junction counting!" << std::endl;
      return 1;
    }

    // Output count table
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Output count table" << std::endl;
    boost::progress_display show_progress( c.nchr.size() );
    std::ofstream intrafile(c.outintra.string().c_str());
    intrafile << "gene\texonA\texonB\t" << c.sampleName << std::endl;
    for(int32_t refIndex=0; refIndex < (int32_t) c.nchr.size(); ++refIndex) {
      ++show_progress;
      if (gRegions[refIndex].empty()) continue;

      // Find chromosome name
      std::string chrname = "NA";
      for(typename CountJunctionConfig::TChrMap::const_iterator itC = c.nchr.begin(); itC != c.nchr.end(); ++itC) {
	if (itC->second == refIndex) {
	  chrname = itC->first;
	  break;
	}
      }

      // Output intra-gene exon-exon junction support
      for(typename TChromosomeRegions::iterator itR = gRegions[refIndex].begin(); itR != gRegions[refIndex].end(); ++itR) {
	typename TChromosomeRegions::iterator itRNext = itR;
	++itRNext;
	for(; itRNext != gRegions[refIndex].end(); ++itRNext) {
	  if ((itR->lid == itRNext->lid) && (itR->end < itRNext->start)) {
	    intrafile << geneIds[itR->lid] << '\t' << chrname << ':' << itR->start << '-' << itR->end << '\t' << chrname << ':' << itRNext->start << '-' << itRNext->end << '\t';
	    int32_t leid = itR->eid;
	    int32_t heid = itRNext->eid;
	    if (leid > heid) {
	      leid = itRNext->eid;
	      heid = itR->eid;
	    }
	    typename TExonJctCount::iterator itE = ejct[refIndex].find(std::make_pair(leid, heid));
	    if (itE != ejct[refIndex].end()) intrafile << itE->second << std::endl;
	    else intrafile << '0' << std::endl;
	  }
	}
      }
    }
    intrafile.close();
    
    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] Done." << std::endl;
    
#ifdef PROFILE
    ProfilerStop();
#endif

    return 0;
  }


  int count_junction(int argc, char **argv) {
    CountJunctionConfig c;

    // Parameter
    boost::program_options::options_description generic("Generic options");
    generic.add_options()
      ("help,?", "show help message")
      ("map-qual,m", boost::program_options::value<unsigned short>(&c.minQual)->default_value(10), "min. mapping quality")
      ("outintra,o", boost::program_options::value<boost::filesystem::path>(&c.outintra)->default_value("intra.tsv"), "intra-gene exon-exon junction reads")
      ("outinter,p", boost::program_options::value<boost::filesystem::path>(&c.outinter)->default_value("inter.tsv"), "inter-gene exon-exon junction reads")
      ;

    boost::program_options::options_description gtfopt("GTF/GFF3 input file options");
    gtfopt.add_options()
      ("gtf,g", boost::program_options::value<boost::filesystem::path>(&c.gtfFile), "gtf/gff3 file")
      ("id,i", boost::program_options::value<std::string>(&c.idname)->default_value("gene_id"), "gtf/gff3 attribute")
      ("feature,f", boost::program_options::value<std::string>(&c.feature)->default_value("exon"), "gtf/gff3 feature")
      ;

    boost::program_options::options_description bedopt("BED input file options, columns chr, start, end, name [, score, strand]");
    bedopt.add_options()
      ("bed,b", boost::program_options::value<boost::filesystem::path>(&c.bedFile), "bed file")
      ;
    
    boost::program_options::options_description hidden("Hidden options");
    hidden.add_options()
      ("input-file", boost::program_options::value<boost::filesystem::path>(&c.bamFile), "input bam file")
      ;

    boost::program_options::positional_options_description pos_args;
    pos_args.add("input-file", -1);

    boost::program_options::options_description cmdline_options;
    cmdline_options.add(generic).add(gtfopt).add(bedopt).add(hidden);
    boost::program_options::options_description visible_options;
    visible_options.add(generic).add(gtfopt).add(bedopt);

    // Parse command-line
    boost::program_options::variables_map vm;
    boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(cmdline_options).positional(pos_args).run(), vm);
    boost::program_options::notify(vm);

    // Check command line arguments
    if ((vm.count("help")) || (!vm.count("input-file")) || ((!vm.count("gtf")) && (!vm.count("bed")))) {
      std::cout << std::endl;
      std::cout << "Usage: alfred " << argv[0] << " [OPTIONS] -g <hg19.gtf.gz> <aligned.bam>" << std::endl;
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
      for(int32_t refIndex=0; refIndex < hdr->n_targets; ++refIndex) c.nchr.insert(std::make_pair(hdr->target_name[refIndex], refIndex));
      
	// Get sample name
      std::string sampleName;
      if (!getSMTag(std::string(hdr->text), c.bamFile.stem().string(), sampleName)) {
	std::cerr << "Only one sample (@RG:SM) is allowed per input BAM file " << c.bamFile.string() << std::endl;
	return 1;
      } else c.sampleName = sampleName;
      bam_hdr_destroy(hdr);
      hts_idx_destroy(idx);
      sam_close(samfile);
    }

    // Check region file
    if (!(boost::filesystem::exists(c.gtfFile) && boost::filesystem::is_regular_file(c.gtfFile) && boost::filesystem::file_size(c.gtfFile))) {
      if (!(boost::filesystem::exists(c.bedFile) && boost::filesystem::is_regular_file(c.bedFile) && boost::filesystem::file_size(c.bedFile))) {
	std::cerr << "Input gtf/bed file is missing." << std::endl;
	return 1;
      } else c.inputFileFormat = 1;
    } else {
      if (is_gff3(c.gtfFile)) c.inputFileFormat = 2;
      else c.inputFileFormat = 0;
    }

    // Show cmd
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] ";
    std::cout << "alfred ";
    for(int i=0; i<argc; ++i) { std::cout << argv[i] << ' '; }
    std::cout << std::endl;

    return countJunctionRun(c);
  }
  


  
}

#endif