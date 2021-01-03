#include <dirent.h>
#include <pthread.h>
#include <math.h>
#include <string>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <omp.h>

#ifdef CPLEX
#include <ilcplex/ilocplex.h>
#endif
#ifdef GUROBI
#include <./gurobi_c++.h>
#endif

#include "query.hpp"
#include "binaryio.hpp"
#include "hashtrie.hpp"

Genome::Genome(uint32_t taxid, std::string &gn) {
	read_cnts_u = 0;
	read_cnts_d = 0;
	glength = 0;
	nus = 0;
	nds = 0;
	taxID = taxid;
	name = gn;
}

FqReader::FqReader(std::string &idx_fn_u, std::string &idx_fn_d, std::string &map_fn, 
		std::string &output_fn, float erate, bool debug_option) {
	ht_u = new Hash(26);
	ht_d = new Hash(26);
	//hash_len_u = hl;
	//hash_len_d = hl;
	IDXFILEU = idx_fn_u;
	IDXFILED = idx_fn_d;
	MAPFILE = map_fn;
	OUTPUTFILE = output_fn;
	erate_ = erate;
	debug_display = debug_option;
}


FqReader::FqReader(uint32_t hl, std::string &idx_fn_u, std::string &idx_fn_d, 
		std::string &map_fn, std::string &output_fn, float erate, bool debug_option) {
	ht_u = new Hash(hl);
	ht_d = new Hash(hl);
	hash_len_u = hl;
	hash_len_d = hl;
	IDXFILEU = idx_fn_u;
	IDXFILED = idx_fn_d;
	MAPFILE = map_fn;
	OUTPUTFILE = output_fn;
	erate_ = erate;
	debug_display = debug_option;
}

FqReader::FqReader(uint32_t hl_u, std::string &idx_fn_u, uint32_t hl_d, std::string &idx_fn_d, 
		std::string &map_fn, std::string &output_fn, float erate, bool debug_option) {
	ht_u = new Hash(hl_u);
	ht_d = new Hash(hl_d);
	hash_len_u = hl_u;
	hash_len_d = hl_d;
	IDXFILEU = idx_fn_u;
	IDXFILED = idx_fn_d;
	MAPFILE = map_fn;
	OUTPUTFILE = output_fn;
	erate_ = erate;
	debug_display = debug_option;
}

void FqReader::clearReads() {
 	if (!reads.empty()) {
		for (size_t i = 0; i < reads.size(); i++) {
			for (auto read : reads[i])
				if (read != NULL)
					delete []read;
			reads[i].clear();
		}
	}
	reads.clear();
}

FqReader::~FqReader() {
	if (ht_u != NULL)
		delete ht_u;
	if (ht_d != NULL)
		delete ht_d;
}

void FqReader::loadIdx_p() {
	auto start = std::chrono::high_resolution_clock::now();

	pthread_t threads[2];
	pthread_create(&threads[0], NULL, loadIdx_u, this);
	pthread_create(&threads[1], NULL, loadIdx_d, this);
	for (int i = 0; i < 2; i++)
		pthread_join(threads[i], NULL);

	assert(hash_len_u > 0 && hash_len_d > 0);		
	fprintf(stderr, "Loaded index files into memory.\n");
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>
					(std::chrono::high_resolution_clock::now() - start).count();
	fprintf(stderr, "Time for loading index: %lu ms.\n", duration);
}

void FqReader::loadSmap() {
	genomes.push_back(NULL);
	std::string line, gname, id, taxid;
	std::ifstream inputFile;

	/* Read in fasta file. */ 
	inputFile.open(MAPFILE);
	
	while (std::getline(inputFile, line)) {
		std::istringstream lines(line);
		while(std::getline(lines, gname, '\t')) {
			std::getline(lines, id, '\t');
			std::getline(lines, taxid, '\t');
			std::getline(lines, gname, '\t');
		}
		Genome *new_genome = new Genome((uint32_t) stoi(taxid), gname);
		genomes.push_back(new_genome);
	}
	inputFile.close();
	fprintf(stderr, "Loaded genome map file.\n");
}

void FqReader::loadGenomeLength() {
	std::string line, id, gl, nu;
	std::ifstream inputGLFile, inputULFile, inputDLFile;

	inputGLFile.open("genome_lengths.out");
	while (std::getline(inputGLFile, line)) {
		std::istringstream lines(line);
		lines >> id;
		lines >> gl;
		genomes[stoi(id)]->glength = ((uint32_t) stoi(gl));
	}
	inputGLFile.close();

	inputULFile.open("unique_lmer_count_u.out");
	while (std::getline(inputULFile, line)) {
		std::istringstream lines(line);
		lines >> id;
		lines >> nu;
		genomes[stoi(id)]->nus = ((uint32_t) stoi(nu));
	}
	inputULFile.close();

	inputDLFile.open("unique_lmer_count_d.out");
	while (std::getline(inputDLFile, line)) {
		std::istringstream lines(line);
		lines >> id;
		lines >> nu;
		genomes[stoi(id)]->nds = ((uint32_t) stoi(nu));
	}
	inputDLFile.close();

	fprintf(stderr, "Loaded genome length file.\n");
}

void FqReader::getFqList(std::string &INDIR) {
	if (!qfilenames.empty()) {
		qfilenames.clear();
		fprintf(stderr, "Flushed existing file names.\n");
	}
	DIR *dir;
	struct dirent *ent;
	if ((dir = opendir(INDIR.c_str())) != NULL) {
		while ((ent = readdir(dir)) != NULL) {
			std::string filename = ent->d_name;
			if (filename.length() >= 7) {
				std::string fext = filename.substr(filename.find_last_of(".") + 1);
				if (fext == "fq" || fext == "fastq")
					qfilenames.push_back(INDIR + filename);
			}
		}
		closedir(dir);
	} else {
		/* could not open directory */
		fprintf(stderr, "Input directory not exists.\n");
		abort();
	}
}

void FqReader::queryFastq_p(std::string &INDIR) {
	getFqList(INDIR);		
	prepallFastq();
	readallFastq();
	loadGenomeLength();
	for (size_t fq_idx = 0; fq_idx < qfilenames.size(); fq_idx++) {
		getFqnameWithoutDir(fq_idx);
		if (nthreads == 1)
			query64_p(100, fq_idx);
		else
			query64mt_p(100, fq_idx);
		#ifdef CPLEX
			runILP_cplex(fq_idx, 100, 100, 10000, erate_, 100.0, 0.0001, 0.01);
		#endif
		#ifdef GUROBI
			runILP_gurobi(fq_idx, 100, 100, 10000, erate_, 100.0, 0.0001, 0.01);
		#endif
		if (fq_idx < qfilenames.size() - 1)
			resetCounters();
	}
}

void FqReader::queryFastq_p(std::vector<std::string> &qfilenames_) {		
	if (!qfilenames.empty()) {
		qfilenames.clear();
		fprintf(stderr, "Flushed existing file names.\n");
	}
	qfilenames = qfilenames_;
	prepallFastq();
	readallFastq();
	loadGenomeLength();
	for (size_t fq_idx = 0; fq_idx < qfilenames_.size(); fq_idx++) {
		getFqnameWithoutDir(fq_idx);
		if (nthreads == 1)
			query64_p(100, fq_idx);
		else
			query64mt_p(100, fq_idx);
		#ifdef CPLEX
			runILP_cplex(fq_idx, 100, 100, 10000, erate_, 100.0, 0.0001, 0.01);
		#endif
		#ifdef GUROBI
			runILP_gurobi(fq_idx, 100, 100, 10000, erate_, 100.0, 0.0001, 0.01);
		#endif
		if (fq_idx < qfilenames_.size() - 1)
			resetCounters();
	}
}

void FqReader::readFastq(std::string &INFILE, size_t file_idx) { 
 	std::string bases, sp;
	std::ifstream inputFile;
	size_t rl;

	/* Read in fasta file. */ 
	inputFile.open(INFILE);
	while (std::getline(inputFile, bases)) {
		std::getline(inputFile, bases);
		rl = bases.length();
		uint8_t *read = new uint8_t[rl];
		strncpy((char*)read, bases.c_str(), rl);
		//reads.push_back(read);
		reads[file_idx].push_back(read);
		//rlengths[file_idx].push_back((uint8_t) rl);
		std::getline(inputFile, bases);
		std::getline(inputFile, bases);
	}
	
	inputFile.close();
	//fprintf(stderr, "%lu\n", reads.size());
}

void FqReader::prepallFastq() {
	for (size_t i = 0; i < qfilenames.size(); i++) {
		//nundet.push_back(0);
		//nconf.push_back(0);
		//genomes->read_cnts_u.push_back(0);
		//genomes->read_cnts_d.push_back(0);
		std::vector<uint8_t*> reads_i;
		reads.push_back(reads_i);
	}
	fprintf(stderr, "Prepared reads from queries.\n");
}

void FqReader::readallFastq() {
	for (size_t i = 0; i < qfilenames.size(); i++)
		readFastq(qfilenames[i], i);
}

void FqReader::getRC(uint8_t *dest, uint8_t *srcs, size_t length) {
	for(size_t i = 0; i < length; i++)
		dest[i] = rcIdx[srcs[length - i - 1]];
}

void FqReader::getFqnameWithoutDir(size_t file_idx) {
	std::stringstream fn_stream(qfilenames[file_idx]);
	while(fn_stream.good())
		getline(fn_stream, current_filename, '/');
}

void FqReader::query64_p(size_t rl, size_t file_idx) {
	auto start = std::chrono::high_resolution_clock::now();
	uint64_t hv = 0;
	uint32_t hs = 0;
	pleafNode *pln;
	std::set<uint32_t> intersection;
	std::set<pleafNode*> pnodes;
	std::set<uint32_t> rids;
	std::set<std::pair<uint32_t, uint32_t>> rid_pairs;
	uint8_t *rc_read = new uint8_t[max_rl];
	size_t nrd = 0;

	fprintf(stderr, "Querying %s.\n", current_filename.c_str());
	for (auto read : reads[file_idx]) {
		rids.clear();
		rid_pairs.clear();
		pnodes.clear();
		intersection.clear();

		/* Forward strand. */
		/* Query unique and doubly-unique substrings. */
		hs = 2 * hash_len_u - 2;
		hv = 0;
		for (size_t i = 0; i < hash_len_u; i++)
			hv = ((hv << 2) | symbolIdx[read[i]]);
		for (size_t i = 0; i < rl - hash_len_u; i++) {
			pln = ht_u->find64_p(hv, read + i + hash_len_u, rl - hash_len_u - i);
			if (pln != NULL)
				pnodes.insert(pln);
			pln = ht_d->find64_p(hv, read + i + hash_len_u, rl - hash_len_u - i);
			if (pln != NULL)
				pnodes.insert(pln);
			hv = hv - ((uint64_t) symbolIdx[read[i]] << hs); /* Next hash. */
			hv = ((hv << 2) | symbolIdx[read[i + hash_len_u]]);
		}
		pln = ht_u->find64_p(hv, read, 0);
		if (pln != NULL)
			pnodes.insert(pln);
		pln = ht_d->find64_p(hv, read, 0);
		if (pln != NULL)
			pnodes.insert(pln);

		/* Reverse complement. */
		getRC(rc_read, read, rl);

		/* Query unique and doubly-unique substrings. */
		hs = 2 * hash_len_u - 2;
		hv = 0;
		for (size_t i = 0; i < hash_len_u; i++)
			hv = ((hv << 2) | symbolIdx[rc_read[i]]);

		for (size_t i = 0; i < rl - hash_len_u; i++) {
			pln = ht_u->find64_p(hv, rc_read + i + hash_len_u, rl - hash_len_u - i);
			if (pln != NULL)
				pnodes.insert(pln);
			pln = ht_d->find64_p(hv, rc_read + i + hash_len_u, rl - hash_len_u - i);
			if (pln != NULL)
				pnodes.insert(pln);
			hv = hv - ((uint64_t) symbolIdx[rc_read[i]] << hs); /* Next hash. */
			hv = ((hv << 2) | symbolIdx[rc_read[i + hash_len_u]]);
		}
		pln = ht_u->find64_p(hv, rc_read, 0);
		if (pln != NULL)
			pnodes.insert(pln);
		pln = ht_d->find64_p(hv, rc_read, 0);
		if (pln != NULL)
			pnodes.insert(pln);

		/* Record results. */
		int i = 0;
		for (auto pn : pnodes) {
			if (pn->refID2 == 0)
				rids.insert(pn->refID1);
			else {
				if (pn->refID1 < pn->refID2)
					rid_pairs.insert(std::make_pair(pn->refID1, pn->refID2));
				else
					rid_pairs.insert(std::make_pair(pn->refID2, pn->refID1));
			}
		}

		switch (rid_pairs.size()) {
			case 0:
				if (rids.empty())
					nundet++;
				else {
					if (rids.size() == 1) {
						for (auto rid : rids)
							genomes[rid]->read_cnts_u++;
						for (auto pn : pnodes)
							pn->rcount += 1;
					} else
						nconf++;
				}
				break;
			case 1:
				if (rids.empty()) {
					for (auto rid_pair : rid_pairs) {
						genomes[rid_pair.first]->read_cnts_d++;
						genomes[rid_pair.second]->read_cnts_d++;
					}
					for (auto pn : pnodes)
						pn->rcount += 1;
				} else {
					if (rids.size() > 1)
						nconf++;
					else {
						for (auto rid : rids)
							for (auto rid_pair : rid_pairs)
								if (rid_pair.first != rid && rid_pair.second != rid) {
									nconf++;
								} else {
									genomes[rid]->read_cnts_u++;
									genomes[rid]->read_cnts_d++;
									for (auto pn : pnodes)
										pn->rcount += 1;
								}
					}
				}
				break;
			default:
				if (!rids.empty()) {
					if (rids.size() > 1) {
						nconf++;
						break;
					}
					for (auto rid : rids) {
						bool conf = 0;
						for (auto rid_pair : rid_pairs)
							if (rid_pair.first != rid && rid_pair.second != rid) {
								conf = 1;
								break;
							}
						if (conf)
							nconf++;
						else {
							genomes[rid]->read_cnts_u++;
							genomes[rid]->read_cnts_d++;
							for (auto pn : pnodes)
								pn->rcount += 1;
						}
					}
				} else {
					i = 0;
					for (auto rid_pair : rid_pairs) {
						if (i == 0) {	
							intersection.insert(rid_pair.first);
							intersection.insert(rid_pair.second);
						} else {
							std::vector<uint32_t> to_be_removed;
							for (auto rid : intersection) {
								if (rid_pair.first != rid && rid_pair.second != rid)
									to_be_removed.push_back(rid);
							}
							for (auto rid : to_be_removed)
								intersection.erase(rid);
						}
						i++;
					}
					switch (intersection.size()) {
						case 0: 
							nconf++;
							break;
						case 1:
							for (auto rid : intersection)
								genomes[rid]->read_cnts_d++;
							for (auto pn : pnodes)
								pn->rcount += 1;
							break;
						default:
							nconf++;
							break;
					}
				}
				break;
		}
		if (nrd++ % 100000 == 0)
			fprintf(stderr, "Processed %lu reads.\r", nrd);
	}

	delete [] rc_read;
	fprintf(stderr, "\nNumber of unlabeled reads: %lu.\n", nundet);
	fprintf(stderr, "Number of reads with conflict labels: %lu.\n", nconf);
	fprintf(stderr, "Completed query %s.\n", current_filename.c_str());
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>
			(std::chrono::high_resolution_clock::now() - start).count();
	fprintf(stderr, "Time for query: %lu ms.\n", duration);
}

void FqReader::query64mt_p(size_t rl, size_t file_idx) {
	auto start = std::chrono::high_resolution_clock::now();
	//fprintf (stderr, "%d\t%d\n", nthreads, omp_get_num_threads());
	//assert(nthreads == omp_get_num_threads());
	std::vector<uint8_t*> rc_read;
	for (int thread_num = 0; thread_num < nthreads; thread_num++) {
		uint8_t *new_read = new uint8_t[max_rl];
		rc_read.push_back(new_read);
	}
	size_t nrd = 0;

	fprintf(stderr, "Querying %s.\n", current_filename.c_str());
	size_t total_nrd = reads[file_idx].size();
	#pragma omp parallel for shared(nrd)
	for (size_t read_idx = 0; read_idx < total_nrd; read_idx++) {
		uint64_t hv = 0;
		uint32_t hs = 0;
		pleafNode *pln;
		std::set<uint32_t> intersection;
		std::set<pleafNode*> pnodes;
		std::set<uint32_t> rids;
		std::set<std::pair<uint32_t, uint32_t>> rid_pairs;
		uint8_t *read = reads[file_idx][read_idx];
		const int thread_id = omp_get_thread_num();

		/* Forward strand. */
		/* Query unique and doubly-unique substrings. */
		hs = 2 * hash_len_u - 2;
		hv = 0;
		for (size_t i = 0; i < hash_len_u; i++)
			hv = ((hv << 2) | symbolIdx[read[i]]);
		for (size_t i = 0; i < rl - hash_len_u; i++) {
			pln = ht_u->find64_p(hv, read + i + hash_len_u, rl - hash_len_u - i);
			if (pln != NULL)
				pnodes.insert(pln);
			pln = ht_d->find64_p(hv, read + i + hash_len_u, rl - hash_len_u - i);
			if (pln != NULL)
				pnodes.insert(pln);
			hv = hv - ((uint64_t) symbolIdx[read[i]] << hs); /* Next hash. */
			hv = ((hv << 2) | symbolIdx[read[i + hash_len_u]]);
		}
		pln = ht_u->find64_p(hv, read, 0);
		if (pln != NULL)
			pnodes.insert(pln);
		pln = ht_d->find64_p(hv, read, 0);
		if (pln != NULL)
			pnodes.insert(pln);

		/* Reverse complement. */
		getRC(rc_read[thread_id], read, rl);

		/* Query unique and doubly-unique substrings. */
		hs = 2 * hash_len_u - 2;
		hv = 0;
		for (size_t i = 0; i < hash_len_u; i++)
			hv = ((hv << 2) | symbolIdx[rc_read[thread_id][i]]);

		for (size_t i = 0; i < rl - hash_len_u; i++) {
			pln = ht_u->find64_p(hv, rc_read[thread_id] + i + hash_len_u, rl - hash_len_u - i);
			if (pln != NULL)
				pnodes.insert(pln);
			pln = ht_d->find64_p(hv, rc_read[thread_id] + i + hash_len_u, rl - hash_len_u - i);
			if (pln != NULL)
				pnodes.insert(pln);
			hv = hv - ((uint64_t) symbolIdx[rc_read[thread_id][i]] << hs); /* Next hash. */
			hv = ((hv << 2) | symbolIdx[rc_read[thread_id][i + hash_len_u]]);
		}
		pln = ht_u->find64_p(hv, rc_read[thread_id], 0);
		if (pln != NULL)
			pnodes.insert(pln);
		pln = ht_d->find64_p(hv, rc_read[thread_id], 0);
		if (pln != NULL)
			pnodes.insert(pln);

		/* Record results. */
		int i = 0;
		for (auto pn : pnodes) {
			if (pn->refID2 == 0)
				rids.insert(pn->refID1);
			else {
				if (pn->refID1 < pn->refID2)
					rid_pairs.insert(std::make_pair(pn->refID1, pn->refID2));
				else
					rid_pairs.insert(std::make_pair(pn->refID2, pn->refID1));
			}
		}

		switch (rid_pairs.size()) {
			case 0:
				if (rids.empty()) {
					#pragma omp critical 
					{
						nundet++;
					}
				} else {
					if (rids.size() == 1) {
						#pragma omp critical 
						{
							for (auto rid : rids)
								genomes[rid]->read_cnts_u++;
							for (auto pn : pnodes)
								pn->rcount += 1;
						}
					} else {
						#pragma omp critical 
						{
							nconf++;
						}
					}
				}
				break;
			case 1:
				if (rids.empty()) {
					#pragma omp critical 
					{
						for (auto rid_pair : rid_pairs) {
							genomes[rid_pair.first]->read_cnts_d++;
							genomes[rid_pair.second]->read_cnts_d++;
						}
						for (auto pn : pnodes)
							pn->rcount += 1;
					}
				} else {
					if (rids.size() > 1) {
						#pragma omp critical 
						{
							nconf++;
						}
					} else {
						for (auto rid : rids)
							for (auto rid_pair : rid_pairs)
								if (rid_pair.first != rid && rid_pair.second != rid) {
									#pragma omp critical 
									{
										nconf++;
									}
								} else {
									#pragma omp critical 
									{
										genomes[rid]->read_cnts_u++;
										genomes[rid]->read_cnts_d++;
										for (auto pn : pnodes)
											pn->rcount += 1;
									}
								}
					}
				}
				break;
			default:
				if (!rids.empty()) {
					if (rids.size() > 1) {
						#pragma omp critical 
						{
							nconf++;
						}
						break;
					}
					for (auto rid : rids) {
						bool conf = 0;
						for (auto rid_pair : rid_pairs)
							if (rid_pair.first != rid && rid_pair.second != rid) {
								conf = 1;
								break;
							}
						if (conf) {
							#pragma omp critical 
							{
								nconf++;
							}
						} else {
							#pragma omp critical 
							{
								genomes[rid]->read_cnts_u++;
								genomes[rid]->read_cnts_d++;
								for (auto pn : pnodes)
									pn->rcount += 1;
							}
						}
					}
				} else {
					i = 0;
					for (auto rid_pair : rid_pairs) {
						if (i == 0) {	
							intersection.insert(rid_pair.first);
							intersection.insert(rid_pair.second);
						} else {
							std::vector<uint32_t> to_be_removed;
							for (auto rid : intersection) {
								if (rid_pair.first != rid && rid_pair.second != rid)
									to_be_removed.push_back(rid);
							}
							for (auto rid : to_be_removed)
								intersection.erase(rid);
						}
						i++;
					}
					switch (intersection.size()) {
						case 0:
							#pragma omp critical 
							{
								nconf++;
							}
							break;
						case 1:
							#pragma omp critical 
							{
								for (auto rid : intersection)
									genomes[rid]->read_cnts_d++;
								for (auto pn : pnodes)
									pn->rcount += 1;
							}
							break;
						default:
							#pragma omp critical 
							{
								nconf++;
							}
							break;
					}
				}
				break;
		}
		#pragma omp critical 
		{
			if (nrd++ % 100000 == 0)
				fprintf(stderr, "Processed %lu reads.\r", nrd);
		}
	}

	for (auto new_read : rc_read)
		delete [] new_read;
	fprintf(stderr, "\nNumber of unlabeled reads: %lu.\n", nundet);
	fprintf(stderr, "Number of reads with conflict labels: %lu.\n", nconf);
	fprintf(stderr, "Completed query %s.\n", current_filename.c_str());
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>
			(std::chrono::high_resolution_clock::now() - start).count();
	fprintf(stderr, "Time for query: %lu ms.\n", duration);
}

#ifdef CPLEX
void FqReader::runILP_cplex(size_t file_idx, int rl, int read_cnt_thres, uint32_t unique_thres, 
			double erate, double max_cov, double resolution, double epsilon) {
	auto start = std::chrono::high_resolution_clock::now();
	
	// Initialize cplex environment. 
	IloEnv env;
	IloModel model(env);
	IloExpr objective(env);
	
	// Binary variable EXIST[l]: Existence of species l.
	size_t n_species = genomes.size() - 1;
	exist = new int[n_species];
	memset(exist, 1, sizeof(int) * n_species);
	IloBoolVarArray EXIST(env, n_species);
	fprintf(stderr, "ILP environment in CPLEX initialized.\n");

	// Constraint 0: If the number of reads being assigned to a particular 
	//	genome g is less than READ_CNT_THRES, then EXIST[g] is set to 0
	for (size_t i = 1; i <= n_species; i++) {
		double d1 = genomes[i]->read_cnts_u * 1.0, d2 = genomes[i]->read_cnts_u * 1.0;
		d1 -= read_cnt_thres;
		d2 -= (1.0 * genomes[i]->nus) * resolution;
		if (genomes[i]->nus >= unique_thres) {
			if (d1 < 0.0 || d2 < 0.0) {
				exist[i - 1] = 0;
				model.add(EXIST[i - 1] == 0);
			} 
		} else {
			if (d2 < 0.0) {
				exist[i - 1] = 0;
				model.add(EXIST[i - 1] == 0);
			}
		}
	}
	for (size_t i = 1; i <= n_species; i++) {
		float d1 = genomes[i]->read_cnts_d * 1.0, d2 = genomes[i]->read_cnts_d * 1.0;
		d1 -= read_cnt_thres;
		d2 -= (1.0 * genomes[i]->nds) * resolution;
		if (genomes[i]->nus >= unique_thres) { // we should try nds?
			if (d1 < 0.0 || d2 < 0.0) {
				exist[i - 1] = 0;
				model.add(EXIST[i - 1] == 0);
			} 
		} else {
			if (d2 < 0.0) {
				exist[i - 1] = 0;
				model.add(EXIST[i - 1] == 0);
			} 
		}
	}

	size_t n_species_exist = 0;
	for (size_t i = 0; i < n_species; i++)
		if (exist[i]) 
			n_species_exist++;
	fprintf(stderr, "%lu genomes may exist in query %s.\n", n_species_exist, current_filename.c_str());

	size_t num_us_valid = 0, abs_index = 0;
	for (size_t i = 0; i < n_species; i++) {
		if (exist[i] > 0) {
			num_us_valid += ht_u->map_sp[i + 1].size();
			num_us_valid += ht_d->map_sp[i + 1].size();
		}
	}
	fprintf(stderr, "Filtered out invalid genomes by read counts.\n");

	// Continuous variable C[l]: Coverage of species l.
	IloNumVarArray COV(env, n_species, 0.0, max_cov, ILOFLOAT);

	// Minimize the total number of species. 
	for (size_t i = 0; i < n_species; i++) {
		double u_factor = 1000.0 / ht_u->map_sp[i + 1].size();
		if (exist[i] > 0) {
			for (auto pn : ht_u->map_sp[i + 1]) {
				assert((i + 1 == pn->refID1) || (i + 1 == pn->refID2));
				double wcov = (pn->ucount1 * (rl - pn->depth) * 1.0 / rl);
				wcov = wcov * pow(1 - erate, pn->depth);
				objective += (wcov * COV[i] - pn->rcount) * (wcov * COV[i] - pn->rcount) * u_factor;
				abs_index++;
			} 
		}
	}
	for (size_t i = 0; i < n_species; i++) {
		double d_factor = 1000.0 / ht_d->map_sp[i + 1].size();
		if (exist[i] > 0) {
			for (auto pn : ht_d->map_sp[i + 1]) {
				assert((i + 1 == pn->refID1) || (i + 1 == pn->refID2));
				uint32_t si1 = pn->refID1 - 1, si2 = pn->refID2 - 1;
				double wcov1 = (pn->ucount1 * (rl - pn->depth) * 1.0 / rl);
				double wcov2 = (pn->ucount2 * (rl - pn->depth) * 1.0 / rl);
				wcov1 = wcov1 * pow(1 - erate, pn->depth);
				wcov2 = wcov2 * pow(1 - erate, pn->depth);
				objective += (wcov1 * COV[si1] + wcov2 * COV[si2] - pn->rcount) * 
						(wcov1 * COV[si1] + wcov2 * COV[si2] - pn->rcount) * d_factor;
				abs_index++;
			} 
		}
	}
	model.add(IloMinimize(env, objective));
	fprintf(stderr, "Constructed the objective function.\n");

	// Constraint 3: COV should match EXIST
	for (size_t i = 0; i < n_species; i++) {
		model.add(COV[i] <= max_cov * EXIST[i]);
		model.add(COV[i] >= 0.01 * EXIST[i]);
	}
	
	IloExprArray EXP1(env, n_species_exist);
	size_t exp_i = 0;
	for (; exp_i < n_species_exist; exp_i++)
		EXP1[exp_i] = IloExpr(env);
	exp_i = 0;
	for (size_t i = 0; i < n_species; i++) {
		if (exist[i] > 0) {
			for (auto pn : ht_u->map_sp[i + 1]) {
				double wcov = (pn->ucount1 * (rl - pn->depth) * 1.0 / rl);
				wcov = wcov * pow(1 - erate, pn->depth);
				EXP1[exp_i] += wcov * COV[i];
			} 
			if (genomes[i + 1]->nus >= unique_thres)
				model.add(EXP1[exp_i] * (1.0 + epsilon) - genomes[i + 1]->read_cnts_u >= 0);
			else
				model.add(EXP1[exp_i] >= 0.0);
			exp_i++;	
		}
	}
	IloExprArray EXP2(env, n_species_exist);
	for (exp_i = 0; exp_i < n_species_exist; exp_i++)
		EXP2[exp_i] = IloExpr(env);

	exp_i = 0;
	for (size_t i = 0; i < n_species; i++) {
		if (exist[i] > 0) {
			for (auto pn : ht_d->map_sp[i + 1]) {
				uint32_t si1 = pn->refID1 - 1, si2 = pn->refID2 - 1;
				double wcov1 = (pn->ucount1 * (rl - pn->depth) * 1.0 / rl);
				double wcov2 = (pn->ucount2 * (rl - pn->depth) * 1.0 / rl);
				wcov1 = wcov1 * pow(1 - erate, pn->depth);
				wcov2 = wcov2 * pow(1 - erate, pn->depth);
				EXP2[exp_i] += wcov1 * COV[si1] + wcov2 * COV[si2];
			}
			if (genomes[i + 1]->nus >= unique_thres)
				model.add(EXP2[exp_i] * (1.0 + epsilon) - genomes[i + 1]->read_cnts_d >= 0);
			else
				model.add(EXP2[exp_i] >= 0.0);
			exp_i++;
		}
	}

	// Constraint 4: refining the search space
	IloExpr TOTAL(env);
	for (size_t i = 0; i < n_species; i++)
		TOTAL += (COV[i] * (1.0 * genomes[i + 1]->glength) / rl);
	model.add(TOTAL <= (1.0 + epsilon) * reads[file_idx].size());

	IloExpr e1(env);
	for (size_t i = 0; i < n_species; i++)
		e1 += EXIST[i];
	model.add(e1 <= n_species_exist * 1.0);
	fprintf(stderr, "Constructed ILP constraints for genome quantification.\n");

	FILE *fout;
	if (file_idx == 0)
		fout = fopen(OUTPUTFILE.c_str(), "w");
	else
		fout = fopen(OUTPUTFILE.c_str(), "a");

	// Solving the ILP.
	try {	
		IloCplex cplex(model);
		if (nthreads > 1)
			cplex.setParam(IloCplex::IntParam::Threads, nthreads);
		cplex.setParam(IloCplex::NumParam::TiLim, 10800);
		cplex.setParam(IloCplex::NumParam::SolnPoolAGap, 0.0);
		if (!debug_display)
			cplex.setOut(env.getNullStream());

		if (cplex.solve()) {
			fprintf(fout, "Query %s:\n", current_filename.c_str());
			fprintf(fout, "TAXID\tABUNDANCE\tNAME\n");
			double total_cov = 0.0;
			std::vector<size_t> cplex_solution;
			for (size_t i = 0; i < n_species; i++) {
				bool ei = cplex.getValue(EXIST[i]);
				double ci = cplex.getValue(COV[i]);
				if (ei != 0) {
					total_cov += ci;
					cplex_solution.push_back(i);
				}
			}
			for (auto si : cplex_solution)
				fprintf(fout, "%u\t%.6f\t%s\n", genomes[si + 1]->taxID,
					cplex.getValue(COV[si]) / total_cov, genomes[si + 1]->name.c_str());
			if (file_idx < qfilenames.size() - 1)
				fprintf(fout, "\n");
		}

		fclose(fout);

	} catch (IloException &ex) {}

	if (exist != NULL)
		delete [] exist;

	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>
			(std::chrono::high_resolution_clock::now() - start).count();
	fprintf(stderr, "Time for quantification through CPLEX: %lu ms.\n", duration);
}
#endif

#ifdef GUROBI
void FqReader::runILP_gurobi(size_t file_idx, int rl, int read_cnt_thres, uint32_t unique_thres, 
			double erate, double max_cov, double resolution, double epsilon) {
	auto start = std::chrono::high_resolution_clock::now();
	
	// Initialize cplex environment.
	GRBEnv *env = new GRBEnv();
	GRBModel model = GRBModel(*env);
	GRBQuadExpr objective = 0;
	
	// Binary variable EXIST[l]: Existence of species l.
	size_t n_species = genomes.size() - 1;
	exist = new int[n_species];
	memset(exist, 1, sizeof(int) * n_species);

	GRBVar *EXIST = model.addVars(n_species, GRB_BINARY);
	fprintf(stderr, "ILP environment in GUROBI initialized.\n");

	// Constraint 0: If the number of reads being assigned to a particular 
	//	genome g is less than READ_CNT_THRES, then EXIST[g] is set to 0
	for (size_t i = 1; i <= n_species; i++) {
		double d1 = genomes[i]->read_cnts_u * 1.0, d2 = genomes[i]->read_cnts_u * 1.0;
		d1 -= read_cnt_thres;
		d2 -= (1.0 * genomes[i]->nus) * resolution;
		if (genomes[i]->nus >= unique_thres) {
			if (d1 < 0.0 || d2 < 0.0) {
				exist[i - 1] = 0;
				model.addConstr(EXIST[i - 1] == 0);
			} 
		} else {
			if (d2 < 0.0) {
				exist[i - 1] = 0;
				model.addConstr(EXIST[i - 1] == 0);
			}
		}
	}
	for (size_t i = 1; i <= n_species; i++) {
		float d1 = genomes[i]->read_cnts_d * 1.0, d2 = genomes[i]->read_cnts_d * 1.0;
		d1 -= read_cnt_thres;
		d2 -= (1.0 * genomes[i]->nds) * resolution;
		if (genomes[i]->nus >= unique_thres) { // we should try nds?
			if (d1 < 0.0 || d2 < 0.0) {
				exist[i - 1] = 0;
				model.addConstr(EXIST[i - 1] == 0);
			} 
		} else {
			if (d2 < 0.0) {
				exist[i - 1] = 0;
				model.addConstr(EXIST[i - 1] == 0);
			} 
		}
	}

	size_t n_species_exist = 0;
	for (size_t i = 0; i < n_species; i++)
		if (exist[i]) 
			n_species_exist++;
	fprintf(stderr, "%lu genomes may exist in query %s.\n", n_species_exist, current_filename.c_str());

	size_t num_us_valid = 0, abs_index = 0;
	for (size_t i = 0; i < n_species; i++) {
		if (exist[i] > 0) {
			num_us_valid += ht_u->map_sp[i + 1].size();
			num_us_valid += ht_d->map_sp[i + 1].size();
		}
	}
	fprintf(stderr, "Filtered out invalid genomes by read counts.\n");

	// Continuous variable C[l]: Coverage of species l.
	GRBVar *COV = model.addVars(n_species, GRB_CONTINUOUS);

	// Minimize the total number of species. 
	for (size_t i = 0; i < n_species; i++) {
		double u_factor = 1000.0 / ht_u->map_sp[i + 1].size();
		if (exist[i] > 0) {
			for (auto pn : ht_u->map_sp[i + 1]) {
				assert((i + 1 == pn->refID1) || (i + 1 == pn->refID2));
				double wcov = (pn->ucount1 * (rl - pn->depth) * 1.0 / rl);
				wcov = wcov * pow(1 - erate, pn->depth);
				objective += (wcov * COV[i] - pn->rcount) * (wcov * COV[i] - pn->rcount) * u_factor;
				abs_index++;
			} 
		}
	}
	for (size_t i = 0; i < n_species; i++) {
		double d_factor = 1000.0 / ht_d->map_sp[i + 1].size();
		if (exist[i] > 0) {
			for (auto pn : ht_d->map_sp[i + 1]) {
				assert((i + 1 == pn->refID1) || (i + 1 == pn->refID2));
				uint32_t si1 = pn->refID1 - 1, si2 = pn->refID2 - 1;
				double wcov1 = (pn->ucount1 * (rl - pn->depth) * 1.0 / rl);
				double wcov2 = (pn->ucount2 * (rl - pn->depth) * 1.0 / rl);
				wcov1 = wcov1 * pow(1 - erate, pn->depth);
				wcov2 = wcov2 * pow(1 - erate, pn->depth);
				objective += (wcov1 * COV[si1] + wcov2 * COV[si2] - pn->rcount) * 
						(wcov1 * COV[si1] + wcov2 * COV[si2] - pn->rcount) * d_factor;
				abs_index++;
			} 
		}
	}
	model.setObjective(objective, GRB_MINIMIZE);
	fprintf(stderr, "Constructed the objective function.\n");

	// Constraint 3: COV should match EXIST
	for (size_t i = 0; i < n_species; i++) {
		model.addConstr(COV[i] <= max_cov * EXIST[i]);
		model.addConstr(COV[i] >= 0.01 * EXIST[i]);
	}
	
	for (size_t i = 0; i < n_species; i++) {
		if (exist[i] > 0) {
			GRBLinExpr EXP1 = 0;
			for (auto pn : ht_u->map_sp[i + 1]) {
				double wcov = (pn->ucount1 * (rl - pn->depth) * 1.0 / rl);
				wcov = wcov * pow(1 - erate, pn->depth);
				EXP1 += wcov * COV[i];
			} 
			if (genomes[i + 1]->nus >= unique_thres)
				model.addConstr(EXP1 * (1.0 + epsilon) - genomes[i + 1]->read_cnts_u >= 0);
			else
				model.addConstr(EXP1 >= 0.0);	
		}
	}
	for (size_t i = 0; i < n_species; i++) {
		if (exist[i] > 0) {
			GRBLinExpr EXP2 = 0;
			for (auto pn : ht_d->map_sp[i + 1]) {
				uint32_t si1 = pn->refID1 - 1, si2 = pn->refID2 - 1;
				double wcov1 = (pn->ucount1 * (rl - pn->depth) * 1.0 / rl);
				double wcov2 = (pn->ucount2 * (rl - pn->depth) * 1.0 / rl);
				wcov1 = wcov1 * pow(1 - erate, pn->depth);
				wcov2 = wcov2 * pow(1 - erate, pn->depth);
				EXP2 += wcov1 * COV[si1] + wcov2 * COV[si2];
			}
			if (genomes[i + 1]->nus >= unique_thres)
				model.addConstr(EXP2 * (1.0 + epsilon) - genomes[i + 1]->read_cnts_d >= 0);
			else
				model.addConstr(EXP2 >= 0.0);
		}
	}

	// Constraint 4: refining the search space
	GRBLinExpr TOTAL = 0;
	for (size_t i = 0; i < n_species; i++)
		TOTAL += (COV[i] * (1.0 * genomes[i + 1]->glength) / rl);
	model.addConstr(TOTAL <= (1.0 + epsilon) * reads[file_idx].size());

	GRBLinExpr e1 = 0;
	for (size_t i = 0; i < n_species; i++)
		e1 += EXIST[i];
	model.addConstr(e1 <= n_species_exist * 1.0);
	fprintf(stderr, "Constructed ILP constraints for genome quantification.\n");

	FILE *fout;
	if (file_idx == 0)
		fout = fopen(OUTPUTFILE.c_str(), "w");
	else
		fout = fopen(OUTPUTFILE.c_str(), "a");

	// Solving the ILP.
	try {
		if (nthreads > 1)
			model.set(GRB_IntParam_Threads, nthreads);
		model.set("TimeLimit", "10800.0");
		model.set("MIPGapAbs", "0.0");
		if (!debug_display)
			model.set(GRB_IntParam_OutputFlag, 0);

		model.optimize();
		//fprintf(stderr, "%d\n", GRB_OPTIMAL);
		if (GRB_OPTIMAL == 2) {
			fprintf(fout, "Query %s:\n", current_filename.c_str());
			fprintf(fout, "TAXID\tABUNDANCE\tNAME\n");
			double total_cov = 0.0;
			std::vector<size_t> grb_solution;
			for (size_t i = 0; i < n_species; i++) {
				bool ei = (EXIST[i].get(GRB_DoubleAttr_X) > 0.9);
				double ci = COV[i].get(GRB_DoubleAttr_X);
				if (ei != 0) {
					total_cov += ci;
					grb_solution.push_back(i);
				}
			}
			for (auto si : grb_solution)
				fprintf(fout, "%u\t%.6f\t%s\n", genomes[si + 1]->taxID,
					COV[si].get(GRB_DoubleAttr_X) / total_cov, genomes[si + 1]->name.c_str());
			if (file_idx < qfilenames.size() - 1)
				fprintf(fout, "\n");
		}

		fclose(fout);
	} catch (GRBException e) {
		fprintf(stderr, "%s\n", e.getMessage().c_str());
		abort();
	}

	delete [] EXIST;
	delete [] COV;
	if (env)
		delete env;
	if (exist != NULL)
		delete [] exist;

	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>
			(std::chrono::high_resolution_clock::now() - start).count();
	fprintf(stderr, "Time for quantification through CPLEX: %lu ms.\n", duration);
}
#endif

void FqReader::resetCounters() {
	auto start = std::chrono::high_resolution_clock::now();
	nconf = 0;
	nundet = 0;

	size_t n_species = genomes.size() - 1;
	for (size_t i = 1; i <= n_species; i++) {
 		genomes[i]->read_cnts_u = 0;
		genomes[i]->read_cnts_d = 0;
	}
	#pragma omp parallel for
	for (size_t i = 1; i <= n_species; i++) {
		for (auto pn : ht_u->map_sp[i])
			pn->rcount = 0;
		for (auto pn : ht_d->map_sp[i])
			pn->rcount = 0;
	}
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>
			(std::chrono::high_resolution_clock::now() - start).count();
	fprintf(stderr, "Time for resetting counters: %lu ms.\n", duration);
}

int FqReader::symbolIdx[256] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, -1, 1, -1, -1, \
-1, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 3, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, 0, -1, 1, -1, -1, -1, 2, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
0, -1, 1, -1, -1, -1, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 3, \
-1, -1, -1, -1, -1, -1};

int FqReader::rcIdx[128] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 84, -1, 71, -1, -1, \
-1, 67, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 65, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, -1, 84, -1, 71, -1, -1, -1, 67, -1, -1, -1, -1, -1, -1, \
-1, -1, -1, -1, -1, -1, 65, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

