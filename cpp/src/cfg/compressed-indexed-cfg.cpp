#include <algorithm>
#include <cstdio>  // FILE
#include <fstream>
#include <stack>
#include <sys/stat.h>
#include "amt/key.hpp"
#include "amt/set.hpp"
#include "cfg/compressed-indexed-cfg.hpp"

#include <iostream>

namespace cfg {

uint64_t CompressedIndexedCFG::getKey(uint8_t* key)
{
    return (uint64_t) amt::get6Int(key);
}

void CompressedIndexedCFG::setKey(uint8_t* key, uint64_t value)
{
    amt::set6Int(key, (uint32_t) value);
}

// construction

CompressedIndexedCFG::CompressedIndexedCFG() { }

// destruction

CompressedIndexedCFG::~CompressedIndexedCFG() {
    delete cset;
    for (int i = 0; i < rulesSize; i++) {
        delete[] rules[i];
    }
    delete[] rules;
}

// construction from MR-Repair grammar

CompressedIndexedCFG* CompressedIndexedCFG::fromMrRepairFile(std::string filename)
{
    CompressedIndexedCFG* cfg = new CompressedIndexedCFG();

    std::ifstream reader(filename);
    std::string line;

    // read grammar specs
    std::getline(reader, line);
    cfg->textLength = std::stoi(line);
    std::getline(reader, line);
    cfg->numRules = std::stoi(line);
    std::getline(reader, line);
    cfg->startSize = std::stoi(line);
    cfg->rulesSize = 0;
    cfg->depth = 0;

    // prepare to read grammar
    cfg->startRule = cfg->numRules + CompressedIndexedCFG::ALPHABET_SIZE;
    int rulesSize = cfg->startRule + 1;  // +1 for start rule
    cfg->rules = new int*[rulesSize];
    int* ruleSizes = new int[rulesSize - 1];
    int* ruleDepths = new int[rulesSize - 1];
    for (int i = 0; i < CompressedIndexedCFG::ALPHABET_SIZE; i++) {
        ruleSizes[i] = 1;
        ruleDepths[i] = 1;
    }
    cfg->rules[cfg->startRule] = new int[cfg->startSize + 1];  // +1 for the dummy code
    int j, c, ruleLength;

    // read rules in order they were added to grammar, i.e. line-by-line
    for (int i = CompressedIndexedCFG::ALPHABET_SIZE; i < cfg->startRule; i++) {
        ruleSizes[i] = 0;
        ruleDepths[i] = 0;
        for (j = 0; ;j++) {
            std::getline(reader, line);
            c = std::stoi(line);
            // use start rule as a buffer
            cfg->rules[cfg->startRule][j] = c;
            if (c == CompressedIndexedCFG::DUMMY_CODE) {
                break;
            }
            ruleSizes[i] += ruleSizes[c];
            ruleDepths[i] = std::max(ruleDepths[c] + 1, ruleDepths[i]);
        }
        ruleLength = j;
        cfg->rulesSize += ruleLength;
        cfg->rules[i] = new int[ruleLength + 1];
        for (j = 0; j < ruleLength + 1; j++) {
            cfg->rules[i][j] = cfg->rules[cfg->startRule][j];
        }
    }

    // read start rule
    amt::Set* set = new amt::Set(cfg->startSize);
    uint64_t pos = 0;
    uint8_t* key = new uint8_t[CompressedIndexedCFG::KEY_LENGTH];
    int len;
    for (int i = 0; i < cfg->startSize; i++) {
        // get the (non-)terminal character
        std::getline(reader, line);
        c = std::stoi(line);
        cfg->rules[cfg->startRule][i] = c;
        // encode a pointer to its index in the AMT Map
        len = amt::set6Int(key, pos);
        //cfg->map.set(key, len, i);
        set->set(key, len);
        pos += ruleSizes[c];
        cfg->depth = std::max(ruleDepths[c] + 1, cfg->depth);
    }
    cfg->rules[cfg->startRule][cfg->startSize] = CompressedIndexedCFG::DUMMY_CODE;
    cfg->cset = new amt::CompressedSumSet(*set, 6, CompressedIndexedCFG::getKey, CompressedIndexedCFG::setKey);

    // clean up
    delete[] key;
    delete set;
    delete[] ruleDepths;
    delete[] ruleSizes;

    return cfg;
}

// construction from Navarro grammar

CompressedIndexedCFG* CompressedIndexedCFG::fromNavarroFiles(std::string filenameC, std::string filenameR)
{
    typedef struct { int left, right; } Tpair;

    CompressedIndexedCFG* cfg = new CompressedIndexedCFG();

    // get the .R file size
    struct stat s;
    stat(filenameR.c_str(), &s);
    int len = s.st_size;

    // open the .R file
    FILE* rFile = fopen(filenameR.c_str(), "r");

    // read the alphabet size
    int alphabetSize;
    fread(&alphabetSize, sizeof(int), 1, rFile);
    cfg->numRules = (len - sizeof(int) - alphabetSize) / sizeof(Tpair);
    cfg->rulesSize = cfg->numRules * 2;  // each rule is a pair
    cfg->startRule = cfg->numRules + CompressedIndexedCFG::ALPHABET_SIZE;

    // read the alphabet map, i.e. \Sigma -> [0..255]
    char map[256];
    fread(&map, sizeof(char), alphabetSize, rFile);

    // prepare to read grammar
    int rulesSize = cfg->startRule + 1;  // +1 for start rule
    cfg->rules = new int*[rulesSize];
    int* ruleSizes = new int[rulesSize - 1];
    int* ruleDepths = new int[rulesSize - 1];
    for (int i = 0; i < CompressedIndexedCFG::ALPHABET_SIZE; i++) {
        ruleSizes[i] = 1;
        ruleDepths[i] = 1;
    }

    // read the rule pairs
    Tpair p;
    int i, c;
    for (int i = CompressedIndexedCFG::ALPHABET_SIZE; i < cfg->startRule; i++) {
        ruleSizes[i] = 0;
        ruleDepths[i] = 0;
        fread(&p, sizeof(Tpair), 1, rFile);
        cfg->rules[i] = new int[3];  // +1 for the dummy code
        if (p.left < alphabetSize) {
            c = (unsigned char) map[p.left];
        } else {
            c = p.left - alphabetSize + CompressedIndexedCFG::ALPHABET_SIZE;
        }
        cfg->rules[i][0] = c;
        ruleSizes[i] += ruleSizes[c];
        ruleDepths[i] = std::max(ruleDepths[c] + 1, ruleDepths[i]);
        if (p.right < alphabetSize) {
            c = (unsigned char) map[p.right];
        } else {
            c = p.right - alphabetSize + CompressedIndexedCFG::ALPHABET_SIZE;
        }
        cfg->rules[i][1] = c;
        ruleSizes[i] += ruleSizes[c];
        ruleDepths[i] = std::max(ruleDepths[c] + 1, ruleDepths[i]);
        cfg->rules[i][2] = CompressedIndexedCFG::DUMMY_CODE;
    }

    // close the .R file
    fclose(rFile);

    // get the .C file size
    stat(filenameC.c_str(), &s);
    cfg->startSize = s.st_size / sizeof(int);
    cfg->rules[cfg->startRule] = new int[cfg->startSize + 1];  // +1 for the dummy code

    // open the .C file
    FILE* cFile = fopen(filenameC.c_str(), "r");

    // read the start rule
    amt::Set* set = new amt::Set(cfg->startSize);
    uint64_t pos = 0;
    uint8_t* key = new uint8_t[CompressedIndexedCFG::KEY_LENGTH];
    int t;
    for (i = 0; i < cfg->startSize; i++) {
        fread(&t, sizeof(int), 1, cFile);
        if (t < alphabetSize) {
            c = (unsigned char) map[t];
        } else {
            c = t - alphabetSize + CompressedIndexedCFG::ALPHABET_SIZE;
        }
        cfg->rules[cfg->startRule][i] = c;
        // encode a pointer to its index in the AMT Map
        len = amt::set6Int(key, pos);
        //cfg->map.set(key, len, i);
        set->set(key, len);
        pos += ruleSizes[c];
        cfg->depth = std::max(ruleDepths[c] + 1, cfg->depth);
    }
    cfg->textLength = pos;
    cfg->rules[cfg->startRule][cfg->startSize] = CompressedIndexedCFG::DUMMY_CODE;
    cfg->cset = new amt::CompressedSumSet(*set, 6, CompressedIndexedCFG::getKey, CompressedIndexedCFG::setKey);

    // clean up
    delete[] key;
    delete set;
    delete[] ruleDepths;
    delete[] ruleSizes;

    return cfg;
}

// construction from BigRePair grammar

CompressedIndexedCFG* CompressedIndexedCFG::fromBigRepairFiles(std::string filenameC, std::string filenameR)
{
    typedef struct { unsigned int left, right; } Tpair;

    CompressedIndexedCFG* cfg = new CompressedIndexedCFG();
    cfg->depth = 0;

    // get the .R file size
    struct stat s;
    stat(filenameR.c_str(), &s);
    int len = s.st_size;

    // open the .R file
    FILE* rFile = fopen(filenameR.c_str(), "r");

    // read the alphabet size
    int alphabetSize;
    fread(&alphabetSize, sizeof(int), 1, rFile);  // NOTE: alphabetSize is always 256
    //cfg->numRules = (len - sizeof(int) - alphabetSize) / sizeof(Tpair);
    cfg->numRules = (len - sizeof(int)) / sizeof(Tpair);
    cfg->rulesSize = cfg->numRules * 2;  // each rule is a pair
    cfg->startRule = cfg->numRules + CompressedIndexedCFG::ALPHABET_SIZE;
    std::cerr << "num rules: " << cfg->numRules << ", start rule: " << cfg->startRule << std::endl;

    // prepare to read grammar
    int rulesSize = cfg->startRule + 1;  // +1 for start rule
    cfg->rules = new int*[rulesSize];
    int* ruleSizes = new int[rulesSize - 1];
    int* ruleDepths = new int[rulesSize - 1];
    for (int i = 0; i < CompressedIndexedCFG::ALPHABET_SIZE; i++) {
        ruleSizes[i] = 0;
        ruleDepths[i] = 1;
    }

    // read the rule pairs
    Tpair p;
    int c;
    for (int i = CompressedIndexedCFG::ALPHABET_SIZE; i < cfg->startRule; i++) {
        ruleSizes[i] = 0;
        ruleDepths[i] = 0;
        fread(&p, sizeof(Tpair), 1, rFile);
        cfg->rules[i] = new int[3];  // +1 for the dummy code
        if (p.left < alphabetSize) {
            if (ruleSizes[p.left] == 0) {
                ruleSizes[p.left] = 1;
            }
            c = (unsigned char) p.left;
        } else {
            c = p.left;  // already offset by alphabetSize
        }
        cfg->rules[i][0] = c;
        ruleSizes[i] += ruleSizes[c];
        ruleDepths[i] = std::max(ruleDepths[c] + 1, ruleDepths[i]);
        if (p.right < alphabetSize) {
            if (ruleSizes[p.right] == 0) {
                ruleSizes[p.right] = 1;
            }
            c = (unsigned char) p.right;
        } else {
            c = p.right;  // already offset by alphabetSize
        }
        cfg->rules[i][1] = c;
        ruleSizes[i] += ruleSizes[c];
        ruleDepths[i] = std::max(ruleDepths[c] + 1, ruleDepths[i]);
        cfg->rules[i][2] = CompressedIndexedCFG::DUMMY_CODE;
    }

    // close the .R file
    fclose(rFile);

    // get the .C file size
    stat(filenameC.c_str(), &s);
    //int c = len = s.st_size / sizeof(unsigned int);
    cfg->startSize = s.st_size / sizeof(unsigned int);
    cfg->rules[cfg->startRule] = new int[cfg->startSize + 1];  // +1 for the dummy code

    // open the .C file
    FILE* cFile = fopen(filenameC.c_str(), "r");

    // read the start rule
    amt::Set* set = new amt::Set(cfg->startSize);
    uint64_t pos = 0;
    uint8_t* key = new uint8_t[CompressedIndexedCFG::KEY_LENGTH];
    int t;
    for (int i = 0; i < cfg->startSize; i++) {
        fread(&t, sizeof(unsigned int), 1, cFile);
        if (t < alphabetSize) {
            c = (unsigned char) t;
        } else {
            c = t;  // already offset by alphabetSize
        }
        cfg->rules[cfg->startRule][i] = c;
        // encode a pointer to its index in the AMT Map
        len = amt::set6Int(key, pos);
        //cfg->map.set(key, len, i);
        set->set(key, len);
        pos += ruleSizes[c];
        cfg->depth = std::max(ruleDepths[c] + 1, cfg->depth);
    }
    cfg->textLength = pos;
    cfg->rules[cfg->startRule][cfg->startSize] = CompressedIndexedCFG::DUMMY_CODE;
    cfg->cset = new amt::CompressedSumSet(*set, 6, CompressedIndexedCFG::getKey, CompressedIndexedCFG::setKey);

    // clean up
    delete[] key;
    delete set;
    delete[] ruleDepths;
    delete[] ruleSizes;

    return cfg;
}

// random access

void CompressedIndexedCFG::get(std::ostream& out, uint32_t begin, uint32_t end)
{
    if (begin < 0 || end >= textLength || begin > end) {
        throw std::runtime_error("begin/end out of bounds");
    }
    uint32_t length = end - begin;

    uint8_t* key = new uint8_t[KEY_LENGTH];
    amt::set6Int(key, begin);
    int i = (int) cset->predecessor(key, KEY_LENGTH);
    //std::cerr << "cset: begin: " << begin << ", i: " << i << std::endl;
    //std::cerr << "begin: " << begin << std::endl;
    //std::cerr << "i: " << i << std::endl;

    //uint32_t predecessor = amt::get6Int(key);
    //uint32_t ignore = begin - predecessor;

    // TODO: stacks should be preallocated to size of max depth
    int r = startRule;
    //std::cerr << "rules[r][i]: " << rules[r][i] << std::endl;
    std::stack<int> ruleStack;
    std::stack<int> indexStack;
    for (uint32_t j = 0; j < length;) {
        // end of rule
        if (rules[r][i] == DUMMY_CODE) {
            r = ruleStack.top();
            ruleStack.pop();
            i = indexStack.top();
            indexStack.pop();
        // terminal character 
        } else if (rules[r][i] < ALPHABET_SIZE) {
            out << (char) rules[r][i];
            i++;
            j++;
        // non-terminal character
        } else {
            ruleStack.push(r);
            r = rules[r][i];
            indexStack.push(i + 1);
            i = 0;
        }
    }
}

}
