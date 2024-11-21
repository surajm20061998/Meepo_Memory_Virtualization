#include <iostream>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <cstdio>
#include <cstdint>
#include <climits>
using namespace std;

const int MAX_VPAGES = 64;
const int MAX_FRAMES = 128;

struct PTE {
    unsigned int present : 1;
    unsigned int referenced : 1;
    unsigned int modified : 1;
    unsigned int write_protect : 1;
    unsigned int paged_out : 1;
    unsigned int frame : 7;
    unsigned int file_mapped : 1;
    unsigned int initialized : 1;
    unsigned int unused : 18;

    PTE() : present(0), referenced(0), modified(0), write_protect(0),
            paged_out(0), frame(0), file_mapped(0), initialized(0), unused(0) {}
};

struct FTE {
    int pid;
    int vpage;
    int index;
    bool occupied;

    FTE(int idx) : pid(-1), vpage(-1), index(idx), occupied(false) {}
};

struct VMA {
    int start_vpage;
    int end_vpage;
    bool write_protect;
    bool file_mapped;

    VMA(int sv, int ev, bool wp, bool fm)
        : start_vpage(sv), end_vpage(ev), write_protect(wp), file_mapped(fm) {}
};

class Process {
public:
    int pid;
    vector<VMA> vmas;
    PTE page_table[MAX_VPAGES];

    struct VPageInfo {
        bool is_valid = false;
        bool write_protect = false;
        bool file_mapped = false;
    };

    VPageInfo vpage_infos[MAX_VPAGES];

    Process(int pid) : pid(pid) {}

    void addVMA(int start, int end, bool wp, bool fm) {
        vmas.emplace_back(start, end, wp, fm);
        for (int i = start; i <= end; ++i) {
            vpage_infos[i].is_valid = true;
            vpage_infos[i].write_protect = wp;
            vpage_infos[i].file_mapped = fm;
        }
    }

    struct Stats {
        unsigned long unmaps = 0;
        unsigned long maps = 0;
        unsigned long ins = 0;
        unsigned long outs = 0;
        unsigned long fins = 0;
        unsigned long fouts = 0;
        unsigned long zeros = 0;
        unsigned long segv = 0;
        unsigned long segprot = 0;
    } pstats;
};

class Pager {
public:
    virtual FTE* select_victim_frame(vector<FTE>& frame_table) = 0;

    // Virtual methods to be overridden by derived classes if needed
    virtual void incrementInstructionCount() {}
    virtual void updateLastUsedTime(int frame_idx, unsigned long long time) {}
    virtual void resetAgeCounter(int frame_idx) {}
    virtual void setProcesses(vector<Process>* procs) {}
    virtual void setOptions(bool a_option) {}
};

class FIFOPager : public Pager {
public:
    FIFOPager() : hand(0) {}

    FTE* select_victim_frame(vector<FTE>& frame_table) override {
        FTE* frame = &frame_table[hand];
        hand = (hand + 1) % frame_table.size();
        return frame;
    }

private:
    size_t hand;
};

class RandomNumberGenerator {
public:
    RandomNumberGenerator(const string& filename) {
        ifstream file(filename);
        if (!file) {
            cerr << "Cannot open random file " << filename << "\n";
            exit(1);
        }
        int count;
        file >> count;
        int val;
        while (file >> val) {
            randvals.push_back(val);
        }
        file.close();
    }

    int getNextRandom(int burst) {
        if (ofs >= randvals.size()) {
            ofs = 0;
        }
        return randvals[ofs++] % burst;
    }

private:
    vector<int> randvals;
    size_t ofs = 0;
};

class RandomPager : public Pager {
public:
    RandomPager(RandomNumberGenerator* rng, int num_frames)
        : rng(rng), num_frames(num_frames) {}

    FTE* select_victim_frame(vector<FTE>& frame_table) override {
        int idx = rng->getNextRandom(num_frames);
        return &frame_table[idx];
    }

private:
    RandomNumberGenerator* rng;
    int num_frames;
};

class ClockPager : public Pager {
public:
    ClockPager() : processes(nullptr), hand(0) {}

    void setProcesses(vector<Process>* procs) override {
        processes = procs;
    }

    FTE* select_victim_frame(vector<FTE>& frame_table) override {
        size_t num_frames = frame_table.size();
        while (true) {
            FTE* frame = &frame_table[hand];
            Process& proc = (*processes)[frame->pid];
            PTE& pte = proc.page_table[frame->vpage];
            if (pte.referenced == 0) {
                FTE* victim = frame;
                hand = (victim->index + 1) % num_frames;
                return victim;
            } else {
                pte.referenced = 0;
            }
            hand = (hand + 1) % num_frames;
        }
    }

private:
    vector<Process>* processes;
    size_t hand;
};

class NRUPager : public Pager {
public:
    NRUPager(vector<Process>* processes, unsigned long long& inst_count)
        : processes(processes), inst_count(inst_count), hand(0), last_reset(0) {}

    void setOptions(bool a_option) override {
        this->a_option = a_option;
    }

    FTE* select_victim_frame(vector<FTE>& frame_table) override;

private:
    vector<Process>* processes;
    unsigned long long& inst_count;
    size_t hand;
    unsigned long long last_reset;
    bool a_option = false;
};

class AgingPager : public Pager {
public:
    AgingPager(vector<Process>* processes, int num_frames)
        : processes(processes), hand(0), num_frames(num_frames) {
        age_counters.resize(num_frames, 0);
    }

    void setOptions(bool a_option) override {
        this->a_option = a_option;
    }

    void resetAgeCounter(int frame_idx) override {
        age_counters[frame_idx] = 0;
    }

    FTE* select_victim_frame(vector<FTE>& frame_table) override;

private:
    vector<Process>* processes;
    size_t hand;
    bool a_option = false;
    int num_frames;
    vector<uint32_t> age_counters;
};

class WorkingSetPager : public Pager {
public:
    WorkingSetPager(vector<Process>* processes, int num_frames)
        : processes(processes), hand(0), instr_count(0), num_frames(num_frames) {
        last_used.resize(num_frames, 0);
    }

    void setOptions(bool a_option) override {
        this->a_option = a_option;
    }

    void incrementInstructionCount() override {
        instr_count++;
    }

    void updateLastUsedTime(int frame_idx, unsigned long long time) override {
        last_used[frame_idx] = time;
    }

    FTE* select_victim_frame(vector<FTE>& frame_table) override;

private:
    vector<Process>* processes;
    size_t hand;
    unsigned long long instr_count;
    bool a_option = false;
    int num_frames;
    vector<unsigned long long> last_used;
};

class MMU {
public:
    MMU(int num_frames, Pager* pager)
        : pager(pager), current_process_id(-1), current_process(nullptr) {
        frame_table.reserve(num_frames);
        for (int i = 0; i < num_frames; ++i) {
            frame_table.emplace_back(i);
            free_frames.push_back(&frame_table.back());
        }
    }

    void setPager(Pager* pager) {
        this->pager = pager;
    }

    void loadInput(const string& filename);
    void simulate();
    void printPageTable();
    void printFrameTable();
    void printSummary();
    void contextSwitch(int procid);
    void handleMemoryAccess(int vpage, bool isWrite);
    void handlePageFault(int vpage);
    void handleProcessExit(int procid);
    FTE* get_frame();
    void setOptions(const string& options);
    vector<Process>& getProcesses() { return processes; }
    const vector<char>& getOutputOptions() const { return output_options; }
    unsigned long long& getInstructionCount() { return inst_count; }

    const int COST_CTX_SWITCH = 130;
    const int COST_PROCESS_EXIT = 1230;
    const int COST_MAP = 350;
    const int COST_UNMAP = 410;
    const int COST_IN = 3200;
    const int COST_OUT = 2750;
    const int COST_FIN = 2350;
    const int COST_FOUT = 2800;
    const int COST_ZERO = 150;
    const int COST_SEGV = 440;
    const int COST_SEGPROT = 410;
    const int COST_READ_WRITE = 1;

private:
    vector<FTE> frame_table;
    Pager* pager;
    vector<Process> processes;
    int current_process_id;
    Process* current_process;
    deque<FTE*> free_frames;
    vector<pair<char, int>> instructions; // Stores parsed instructions
    unsigned long long inst_count = 0;
    unsigned long long total_cost = 0;
    unsigned long ctx_switches = 0;
    unsigned long process_exits = 0;

    bool O_option = false;
    bool P_option = false;
    bool F_option = false;
    bool S_option = false;
    bool a_option = false;
    bool x_option = false;
    bool y_option = false;
    bool f_option = false;

    vector<char> output_options;

    void printCurrentProcessPageTable();
};

void MMU::setOptions(const string& options) {
    for (char ch : options) {
        switch (ch) {
            case 'O':
            case 'P':
            case 'F':
            case 'S':
            case 'x':
            case 'y':
            case 'f':
            case 'a':
                output_options.push_back(ch);
                break;
            default:
                break;
        }

        switch (ch) {
            case 'O':
                O_option = true;
                break;
            case 'P':
                P_option = true;
                break;
            case 'F':
                F_option = true;
                break;
            case 'S':
                S_option = true;
                break;
            case 'x':
                x_option = true;
                break;
            case 'y':
                y_option = true;
                break;
            case 'f':
                f_option = true;
                break;
            case 'a':
                a_option = true;
                break;
            default:
                break;
        }
    }
}

void MMU::loadInput(const string& filename) {
    ifstream file(filename);
    string line;

    if (!file.is_open()) {
        cerr << "Error opening file" << "\n";
        exit(1);
    }

    while (getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        else break;
    }

    int num_processes = stoi(line);
    processes.reserve(num_processes);

    for (int pid = 0; pid < num_processes; ++pid) {

        while (getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            else break;
        }
        if (line.empty()) break;
        int num_vmas = stoi(line);
        Process proc(pid);
        for (int i = 0; i < num_vmas; ++i) {
            int start_vpage, end_vpage, wp, fm;
            while (getline(file, line)) {
                if (line.empty() || line[0] == '#') continue;
                else break;
            }
            istringstream iss(line);
            iss >> start_vpage >> end_vpage >> wp >> fm;
            proc.addVMA(start_vpage, end_vpage, wp, fm);
        }
        processes.push_back(proc);
    }

    // Read and parse instructions
    while (getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        istringstream iss(line);
        char operation;
        int value;
        iss >> operation >> value;
        instructions.emplace_back(operation, value); // Store parsed instruction
    }

    file.close();
}

void MMU::simulate() {
    for (const auto& instr : instructions) {
        char operation = instr.first;
        int value = instr.second;
        inst_count++;

        if (O_option) {
            cout << (inst_count - 1) << ": ==> " << operation << " " << value << "\n";
        }

        switch (operation) {
            case 'c':
                contextSwitch(value);
                break;
            case 'r':
                handleMemoryAccess(value, false);
                break;
            case 'w':
                handleMemoryAccess(value, true);
                break;
            case 'e':
                handleProcessExit(value);
                break;
            default:
                cerr << "Error: Unknown operation " << operation << "\n";
                break;
        }

        // Call incrementInstructionCount for pagers that need it
        pager->incrementInstructionCount();

        if (x_option || y_option || f_option) {

            for (char opt : output_options) {
                switch (opt) {
                    case 'x':
                        if (x_option) {
                            printCurrentProcessPageTable();
                        }
                        break;
                    case 'y':
                        if (y_option) {
                            printPageTable();
                        }
                        break;
                    case 'f':
                        if (f_option) {
                            printFrameTable();
                        }
                        break;
                    default:
                        break;
                }
            }
        }
    }
}

void MMU::contextSwitch(int procid) {
    if (current_process_id != procid) {
        ctx_switches++;
        total_cost += COST_CTX_SWITCH;
    }
    current_process_id = procid;
    current_process = &processes[procid];
}

void MMU::handleMemoryAccess(int vpage, bool isWrite) {
    total_cost += COST_READ_WRITE;

    if (vpage < 0 || vpage >= MAX_VPAGES) {
        if (O_option) cout << " SEGV" << "\n";
        current_process->pstats.segv++;
        total_cost += COST_SEGV;
        return;
    }

    PTE& pte = current_process->page_table[vpage];

    if (!pte.present) {
        handlePageFault(vpage);
        if (!pte.present) return;
    }

    if (isWrite && pte.write_protect) {
        if (O_option) cout << " SEGPROT" << "\n";
        pte.referenced = 1;
        current_process->pstats.segprot++;
        total_cost += COST_SEGPROT;
        return;
    }

    pte.referenced = 1;
    if (isWrite) {
        pte.modified = 1;
    }

    // Update last used time for pagers that need it
    pager->updateLastUsedTime(pte.frame, inst_count);
}

void MMU::handlePageFault(int vpage) {
    if (!current_process->vpage_infos[vpage].is_valid) {
        if (O_option) cout << " SEGV" << "\n";
        current_process->pstats.segv++;
        total_cost += COST_SEGV;
        return;
    }

    FTE* frame = get_frame();

    if (frame->occupied) {
        Process& old_process = processes[frame->pid];
        PTE& old_pte = old_process.page_table[frame->vpage];

        if (O_option) cout << " UNMAP " << frame->pid << ":" << frame->vpage << "\n";
        old_process.pstats.unmaps++;
        total_cost += COST_UNMAP;

        if (old_pte.modified) {
            if (old_pte.file_mapped) {
                if (O_option) cout << " FOUT" << "\n";
                old_process.pstats.fouts++;
                total_cost += COST_FOUT;
            } else {
                if (O_option) cout << " OUT" << "\n";
                old_process.pstats.outs++;
                total_cost += COST_OUT;
                old_pte.paged_out = 1;
            }
            old_pte.modified = 0;
        }

        old_pte.present = 0;
        old_pte.frame = 0;
        old_pte.referenced = 0;

        frame->occupied = false;
        frame->pid = -1;
        frame->vpage = -1;
    }

    frame->occupied = true;
    frame->pid = current_process_id;
    frame->vpage = vpage;

    PTE& pte = current_process->page_table[vpage];
    pte.present = 1;
    pte.frame = frame->index;

    if (!pte.initialized) {
        pte.write_protect = current_process->vpage_infos[vpage].write_protect;
        pte.file_mapped = current_process->vpage_infos[vpage].file_mapped;
        pte.initialized = 1;
    }

    if (pte.file_mapped) {
        if (O_option) cout << " FIN" << "\n";
        current_process->pstats.fins++;
        total_cost += COST_FIN;
    } else {
        if (pte.paged_out) {
            if (O_option) cout << " IN" << "\n";
            current_process->pstats.ins++;
            total_cost += COST_IN;
        } else {
            if (O_option) cout << " ZERO" << "\n";
            current_process->pstats.zeros++;
            total_cost += COST_ZERO;
        }
    }

    if (O_option) cout << " MAP " << pte.frame << "\n";
    current_process->pstats.maps++;
    total_cost += COST_MAP;

    // Reset age counter for pagers that need it
    pager->resetAgeCounter(pte.frame);

    // Update last used time for pagers that need it
    pager->updateLastUsedTime(pte.frame, inst_count);
}

void MMU::handleProcessExit(int procid) {
    Process& proc = processes[procid];

    if (O_option) cout << "EXIT current process " << procid << "\n";

    for (int i = 0; i < MAX_VPAGES; ++i) {
        PTE& pte = proc.page_table[i];
        if (pte.present) {
            FTE& frame = frame_table[pte.frame];
            if (O_option) cout << " UNMAP " << procid << ":" << i << "\n";
            proc.pstats.unmaps++;
            total_cost += COST_UNMAP;

            if (pte.modified) {
                if (pte.file_mapped) {
                    if (O_option) cout << " FOUT" << "\n";
                    proc.pstats.fouts++;
                    total_cost += COST_FOUT;
                }
            }

            frame.occupied = false;
            frame.pid = -1;
            frame.vpage = -1;
            free_frames.push_back(&frame);

            pte.present = 0;
            pte.referenced = 0;
            pte.modified = 0;
            pte.frame = 0;
            pte.paged_out = 0;
        } else {
            pte.paged_out = 0;
        }
    }

    process_exits++;
    total_cost += COST_PROCESS_EXIT;
}

FTE* MMU::get_frame() {
    if (!free_frames.empty()) {
        FTE* frame = free_frames.front();
        free_frames.pop_front();
        return frame;
    }
    FTE* frame = pager->select_victim_frame(frame_table);
    return frame;
}

void MMU::printPageTable() {
    if (P_option) {
        for (const auto& process : processes) {
            cout << "PT[" << process.pid << "]: ";
            bool first = true;
            for (int i = 0; i < MAX_VPAGES; ++i) {
                PTE pte = process.page_table[i];
                std::string output;

                if (pte.present) {
                    output = std::to_string(i) + ":";
                    output += (pte.referenced ? "R" : "-");
                    output += (pte.modified ? "M" : "-");
                    output += (pte.paged_out ? "S" : "-");
                } else {
                    output = (pte.paged_out) ? "#" : "*";
                }

                if (!first) {
                    cout << " ";
                } else {
                    first = false;
                }

                cout << output;
            }
            cout << "\n";
        }
    }
}

void MMU::printCurrentProcessPageTable() {
    cout << "PT[" << current_process->pid << "]: ";
    for (int i = 0; i < MAX_VPAGES; ++i) {
        PTE pte = current_process->page_table[i];
        if (pte.present) {
            cout << i << ":";
            cout << (pte.referenced ? "R" : "-");
            cout << (pte.modified ? "M" : "-");
            cout << (pte.paged_out ? "S" : "-") << " ";
        } else {
            if (pte.paged_out) {
                cout << "# ";
            } else {
                cout << "* ";
            }
        }
    }
    cout << "\n";
}

void MMU::printFrameTable() {
    if (F_option || f_option) {
        cout << "FT: ";
        bool first = true;
        for (size_t i = 0; i < frame_table.size(); ++i) {
            FTE& frame = frame_table[i];
            std::string output;

            if (frame.occupied) {
                output = std::to_string(frame.pid) + ":" + std::to_string(frame.vpage);
            } else {
                output = "*";
            }

            if (!first) {
                cout << " ";
            } else {
                first = false;
            }

            cout << output;
        }
        cout << "\n";
    }
}

void MMU::printSummary() {
    if (S_option) {
        for (const auto& proc : processes) {
            printf("PROC[%d]: U=%lu M=%lu I=%lu O=%lu FI=%lu FO=%lu Z=%lu SV=%lu SP=%lu\n",
                   proc.pid,
                   proc.pstats.unmaps,
                   proc.pstats.maps,
                   proc.pstats.ins,
                   proc.pstats.outs,
                   proc.pstats.fins,
                   proc.pstats.fouts,
                   proc.pstats.zeros,
                   proc.pstats.segv,
                   proc.pstats.segprot);
        }
        printf("TOTALCOST %llu %lu %lu %llu %lu\n",
               inst_count, ctx_switches, process_exits, total_cost, sizeof(PTE));
    }
}

FTE* NRUPager::select_victim_frame(vector<FTE>& frame_table) {
    size_t num_frames = frame_table.size();
    bool reset_referenced = false;
    int lowest_class = 4;
    FTE* victim = nullptr;
    FTE* class_frames[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t start_hand = hand;

    if (inst_count - last_reset >= 48) {
        reset_referenced = true;
        last_reset = inst_count;
    }

    do {
        FTE* frame = &frame_table[hand];

        Process& proc = (*processes)[frame->pid];
        PTE& pte = proc.page_table[frame->vpage];

        int class_idx = 2 * pte.referenced + pte.modified;

        if (!class_frames[class_idx]) {
            class_frames[class_idx] = frame;
            if (class_idx < lowest_class) {
                lowest_class = class_idx;
            }
        }

        if (reset_referenced) {
            pte.referenced = 0;
        }

        hand = (hand + 1) % num_frames;

        if (!reset_referenced && class_idx == 0) {
            break;
        }
    } while (hand != start_hand);

    if (lowest_class == 4) {
        cerr << "NRU pager error: No victim frame found" << "\n";
        exit(1);
    }

    victim = class_frames[lowest_class];

    hand = (victim->index + 1) % num_frames;

    if (a_option) {
        cout << "ASELECT: " << start_hand << " " << (reset_referenced ? 1 : 0)
             << " | " << lowest_class << " " << victim->index << "\n";
    }

    return victim;
}

FTE* AgingPager::select_victim_frame(vector<FTE>& frame_table) {
    size_t num_frames = frame_table.size();
    uint32_t min_age = UINT32_MAX;
    FTE* victim = nullptr;
    size_t start_hand = hand;

    if (a_option) {
        cout << "ASELECT " << hand << "-" << ((hand + num_frames - 1) % num_frames) << " | ";
    }

    for (size_t i = 0; i < num_frames; ++i) {
        FTE* frame = &frame_table[hand];
        Process& proc = (*processes)[frame->pid];
        PTE& pte = proc.page_table[frame->vpage];

        age_counters[hand] >>= 1;
        if (pte.referenced) {
            age_counters[hand] |= 0x80000000;
            pte.referenced = 0;
        }

        if (a_option) {
            cout << hand << ":" << hex << age_counters[hand] << " ";
        }

        if (age_counters[hand] < min_age) {
            min_age = age_counters[hand];
            victim = frame;
        }

        hand = (hand + 1) % num_frames;
    }

    hand = (victim->index + 1) % num_frames;

    if (a_option) {
        cout << "| " << victim->index << "\n";
    }

    return victim;
}

FTE* WorkingSetPager::select_victim_frame(vector<FTE>& frame_table) {
    size_t num_frames = frame_table.size();
    unsigned long long oldest_time = instr_count;
    FTE* victim = nullptr;
    size_t frames_scanned = 0;
    size_t start_hand = hand;
    const int TAU = 50;

    if (a_option) {
        cout << "ASELECT " << hand << "-" << ((hand + num_frames - 1) % num_frames) << " | ";
    }

    do {
        FTE* frame = &frame_table[hand];
        Process& proc = (*processes)[frame->pid];
        PTE& pte = proc.page_table[frame->vpage];

        frames_scanned++;

        if (pte.referenced) {
            pte.referenced = 0;
            last_used[hand] = instr_count;
        } else {
            if (instr_count - last_used[hand] >= TAU) {
                victim = frame;
                if (a_option) {
                    cout << hand << "(" << pte.referenced << " " << frame->pid << ":" << frame->vpage << " "
                              << last_used[hand] << ") ";
                }
                break;
            }
        }

        if (!victim || last_used[hand] < oldest_time) {
            oldest_time = last_used[hand];
            victim = frame;
        }

        if (a_option) {
            cout << hand << "(" << pte.referenced << " " << frame->pid << ":" << frame->vpage << " "
                      << last_used[hand] << ") ";
        }

        hand = (hand + 1) % num_frames;
    } while (hand != start_hand);

    if (a_option) {
        cout << "| " << victim->index << "\n";
    }

    hand = (victim->index + 1) % num_frames;

    return victim;
}

int main(int argc, char* argv[]) {
    int num_frames = 128;
    Pager* pager = nullptr;
    string algo = "f";
    string options;
    string inputfile;
    string randomfile;

    int c;
    while ((c = getopt(argc, argv, "f:a:o:")) != -1) {
        switch (c) {
            case 'f':
                num_frames = atoi(optarg);
                break;
            case 'a':
                algo = optarg;
                break;
            case 'o':
                options = optarg;
                break;
            default:
                cerr << "Unknown option " << c << "\n";
                return 1;
        }
    }

    if (optind + 2 > argc) {
        cerr << "Error: Missing inputfile and/or randomfile" << "\n";
        return 1;
    }

    inputfile = argv[optind];
    randomfile = argv[optind + 1];

    RandomNumberGenerator randGen(randomfile);

    MMU mmu(num_frames, nullptr);
    mmu.setOptions(options);
    mmu.loadInput(inputfile);

    bool a_option = false;
    for (char ch : options) {
        if (ch == 'a') {
            a_option = true;
            break;
        }
    }

    if (algo == "f") {
        pager = new FIFOPager();
    } else if (algo == "r") {
        pager = new RandomPager(&randGen, num_frames);
    } else if (algo == "c") {
        pager = new ClockPager();
    } else if (algo == "e") {
        pager = new NRUPager(&mmu.getProcesses(), mmu.getInstructionCount());
    } else if (algo == "a") {
        pager = new AgingPager(&mmu.getProcesses(), num_frames);
    } else if (algo == "w") {
        pager = new WorkingSetPager(&mmu.getProcesses(), num_frames);
    } else {
        cerr << "Unknown algorithm '" << algo << "'" << "\n";
        return 1;
    }

    // Set options and processes for pagers that need them
    pager->setOptions(a_option);
    pager->setProcesses(&mmu.getProcesses());

    mmu.setPager(pager);

    mmu.simulate();

    const vector<char>& output_options = mmu.getOutputOptions();
    for (char opt : output_options) {
        switch (opt) {
            case 'P':
                mmu.printPageTable();
                break;
            case 'F':
                mmu.printFrameTable();
                break;
            case 'S':
                mmu.printSummary();
                break;
            default:
                break;
        }
    }

    delete pager;
    return 0;
}
