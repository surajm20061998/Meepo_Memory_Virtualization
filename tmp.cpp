#include <iostream>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>  // For getopt()
#include <cstdio>    // For printf
#include <cstdint>   // For fixed-width integer types
#include <climits>   // For UINT32_MAX

// Constants
const int MAX_VPAGES = 64;
const int MAX_FRAMES = 128;

// Define Page Table Entry (PTE) with bit-fields
struct PTE {
    unsigned int present : 1;
    unsigned int referenced : 1;
    unsigned int modified : 1;
    unsigned int write_protect : 1;
    unsigned int paged_out : 1;
    unsigned int frame : 7;  // 128 frames max (7 bits)
    unsigned int file_mapped : 1;  // Additional bit for file-mapped pages
    unsigned int initialized : 1;  // Indicates if VMA bits have been set
    unsigned int unused : 18;  // Remaining bits

    PTE() : present(0), referenced(0), modified(0), write_protect(0),
            paged_out(0), frame(0), file_mapped(0), initialized(0), unused(0) {}
};

// Frame Table Entry
struct FTE {
    int pid;   // Process ID using this frame
    int vpage; // Virtual page mapped to this frame
    bool occupied; // Whether the frame is occupied

    FTE() : pid(-1), vpage(-1), occupied(false) {}
};

// Virtual Memory Area (VMA)
struct VMA {
    int start_vpage;
    int end_vpage;
    bool write_protect;
    bool file_mapped;

    VMA(int sv, int ev, bool wp, bool fm)
        : start_vpage(sv), end_vpage(ev), write_protect(wp), file_mapped(fm) {}
};

// Process class to hold VMAs and page table
class Process {
public:
    int pid;
    std::vector<VMA> vmas;
    PTE page_table[MAX_VPAGES];

    Process(int pid) : pid(pid) {}

    void addVMA(int start, int end, bool wp, bool fm) {
        vmas.emplace_back(start, end, wp, fm);
    }

    bool isPageInVMA(int vpage) const {
        for (const auto& vma : vmas) {
            if (vpage >= vma.start_vpage && vpage <= vma.end_vpage)
                return true;
        }
        return false;
    }

    VMA getVMA(int vpage) const {
        for (const auto& vma : vmas) {
            if (vpage >= vma.start_vpage && vpage <= vma.end_vpage)
                return vma;
        }
        // Should not reach here if isPageInVMA is checked before
        return VMA(0, 0, 0, 0);
    }

    // Statistics
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

// Pager base class for page replacement algorithms
class Pager {
public:
    virtual FTE* select_victim_frame(std::vector<FTE>& frame_table) = 0;
};

// FIFO Pager implementation
class FIFOPager : public Pager {
public:
    FIFOPager() : hand(0) {}

    FTE* select_victim_frame(std::vector<FTE>& frame_table) override {
        FTE* frame = &frame_table[hand];
        hand = (hand + 1) % frame_table.size();
        return frame;
    }

private:
    size_t hand;
};

// Random Number Generator
class RandomNumberGenerator {
public:
    RandomNumberGenerator(const std::string& filename) {
        std::ifstream file(filename);
        if (!file) {
            std::cerr << "Cannot open random file " << filename << "\n";
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
    std::vector<int> randvals;
    size_t ofs = 0;
};

// Random Pager
class RandomPager : public Pager {
public:
    RandomPager(RandomNumberGenerator* rng, int num_frames)
        : rng(rng), num_frames(num_frames) {}

    FTE* select_victim_frame(std::vector<FTE>& frame_table) override {
        int idx = rng->getNextRandom(num_frames);
        return &frame_table[idx];
    }

private:
    RandomNumberGenerator* rng;
    int num_frames;
};

// Clock Pager
class ClockPager : public Pager {
public:
    ClockPager() : processes(nullptr), hand(0) {}
    
    void setProcesses(std::vector<Process>* procs) {
        processes = procs;
    }

    FTE* select_victim_frame(std::vector<FTE>& frame_table) override {
        size_t num_frames = frame_table.size();
        while (true) {
            FTE* frame = &frame_table[hand];
            Process& proc = (*processes)[frame->pid];
            PTE& pte = proc.page_table[frame->vpage];
            if (pte.referenced == 0) {
                // Victim frame found
                FTE* victim = frame;
                hand = (hand + 1) % num_frames;
                return victim;
            } else {
                // Reset referenced bit
                pte.referenced = 0;
                hand = (hand + 1) % num_frames;
            }
        }
    }

private:
    std::vector<Process>* processes;
    size_t hand;
};

// NRU Pager (Enhanced Second Chance)
class NRUPager : public Pager {
public:
    NRUPager(std::vector<Process>* processes)
        : processes(processes), hand(0), instr_count(0), last_reset(0) {}

    void setOptions(bool a_option) {
        this->a_option = a_option;
    }

    void incrementInstructionCount() {
        instr_count++;
    }

    FTE* select_victim_frame(std::vector<FTE>& frame_table) override;

private:
    std::vector<Process>* processes;
    size_t hand;
    unsigned long long instr_count;
    unsigned long long last_reset;
    bool a_option = false;
};

// Aging Pager
class AgingPager : public Pager {
public:
    AgingPager(std::vector<Process>* processes, int num_frames)
        : processes(processes), hand(0), num_frames(num_frames) {
        age_counters.resize(num_frames, 0);
    }

    void setOptions(bool a_option) {
        this->a_option = a_option;
    }

    FTE* select_victim_frame(std::vector<FTE>& frame_table) override;

    // Add this public method
    void resetAgeCounter(int frame_idx) {
        age_counters[frame_idx] = 0;
    }

private:
    std::vector<Process>* processes;
    size_t hand;
    bool a_option = false;
    int num_frames;
    std::vector<uint32_t> age_counters;  // Remains private
};

// Working Set Pager
class WorkingSetPager : public Pager {
public:
    WorkingSetPager(std::vector<Process>* processes, int num_frames)
        : processes(processes), hand(0), instr_count(0), num_frames(num_frames) {
        last_used.resize(num_frames, 0);
    }

    void setOptions(bool a_option) {
        this->a_option = a_option;
    }

    void incrementInstructionCount() {
        instr_count++;
    }

    void updateLastUsedTime(int frame_idx, unsigned long long time) {
        last_used[frame_idx] = time;
    }

    FTE* select_victim_frame(std::vector<FTE>& frame_table) override;

private:
    std::vector<Process>* processes;
    size_t hand;
    unsigned long long instr_count;
    bool a_option = false;
    int num_frames;
    std::vector<unsigned long long> last_used;
};

// MMU Simulation Class
class MMU {
public:
    MMU(int num_frames, Pager* pager)
        : frame_table(num_frames), pager(pager), current_process_id(-1), current_process(nullptr) {
        // Initialize free frames
        for (int i = 0; i < num_frames; ++i) {
            free_frames.push_back(&frame_table[i]);
        }
    }

    void setPager(Pager* pager) {
        this->pager = pager;
    }

    void loadInput(const std::string& filename);
    void simulate();
    void printPageTable();
    void printFrameTable();
    void printSummary();
    void contextSwitch(int procid);
    void handleMemoryAccess(int vpage, bool isWrite);
    void handlePageFault(int vpage);
    void handleProcessExit(int procid);
    FTE* get_frame();
    void setOptions(const std::string& options);
    std::vector<Process>& getProcesses() { return processes; }

    // Operation costs
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
    std::vector<FTE> frame_table;
    Pager* pager;
    std::vector<Process> processes;
    int current_process_id;
    Process* current_process;
    std::deque<FTE*> free_frames;
    std::vector<std::string> instruction_lines;
    unsigned long long inst_count = 0;
    unsigned long long total_cost = 0;
    unsigned long ctx_switches = 0;
    unsigned long process_exits = 0;

    // Options
    bool O_option = false;
    bool P_option = false;
    bool F_option = false;
    bool S_option = false;
    bool a_option = false;
};

// Function to set options
void MMU::setOptions(const std::string& options) {
    for (char ch : options) {
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
            case 'a':
                a_option = true;
                break;
            default:
                break;
        }
    }
}

// Function to load and parse input
void MMU::loadInput(const std::string& filename) {
    std::ifstream file(filename);
    std::string line;

    if (!file.is_open()) {
        std::cerr << "Error opening file\n";
        exit(1);
    }

    // Skip comments at the beginning
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        else break;
    }

    int num_processes = std::stoi(line);
    processes.reserve(num_processes);

    for (int pid = 0; pid < num_processes; ++pid) {
        // Skip comments
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            else break;
        }
        if (line.empty()) break;
        int num_vmas = std::stoi(line);
        Process proc(pid);
        for (int i = 0; i < num_vmas; ++i) {
            int start_vpage, end_vpage, wp, fm;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') continue;
                else break;
            }
            std::istringstream iss(line);
            iss >> start_vpage >> end_vpage >> wp >> fm;
            proc.addVMA(start_vpage, end_vpage, wp, fm);
        }
        processes.push_back(proc);
    }

    // Now read instruction lines
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        instruction_lines.push_back(line);
    }

    file.close();
}

// Main simulation loop
void MMU::simulate() {
    for (const auto& line : instruction_lines) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        char operation;
        int value;
        iss >> operation >> value;

        if (O_option) {
            std::cout << inst_count << ": ==> " << operation << " " << value << "\n";
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
                std::cerr << "Error: Unknown operation " << operation << "\n";
                break;
        }

        inst_count++;

        // Update instruction count for pagers that need it
        if (auto nruPager = dynamic_cast<NRUPager*>(pager)) {
            nruPager->incrementInstructionCount();
        }
        if (auto wsPager = dynamic_cast<WorkingSetPager*>(pager)) {
            wsPager->incrementInstructionCount();
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
        if (O_option) std::cout << " SEGV\n";
        current_process->pstats.segv++;
        total_cost += COST_SEGV;
        return;
    }

    PTE& pte = current_process->page_table[vpage];

    if (!pte.present) {
        handlePageFault(vpage);
        if (!pte.present) return;  // SEGV occurred
    }

    if (isWrite && pte.write_protect) {
        if (O_option) std::cout << " SEGPROT\n";
        pte.referenced = 1;
        current_process->pstats.segprot++;
        total_cost += COST_SEGPROT;
        return;
    }

    // Simulate setting the referenced and modified bits
    pte.referenced = 1;
    if (isWrite) {
        pte.modified = 1;
    }

    // Update last used time for Working Set Pager
    if (auto wsPager = dynamic_cast<WorkingSetPager*>(pager)) {
        size_t frame_idx = pte.frame;
        wsPager->updateLastUsedTime(frame_idx, inst_count);
    }
}

void MMU::handlePageFault(int vpage) {
    if (!current_process->isPageInVMA(vpage)) {
        if (O_option) std::cout << " SEGV\n";
        current_process->pstats.segv++;
        total_cost += COST_SEGV;
        return;
    }

    FTE* frame = get_frame();

    // Unmap existing page if needed
    if (frame->occupied) {
        Process& old_process = processes[frame->pid];
        PTE& old_pte = old_process.page_table[frame->vpage];

        if (O_option) std::cout << " UNMAP " << frame->pid << ":" << frame->vpage << "\n";
        old_process.pstats.unmaps++;
        total_cost += COST_UNMAP;

        if (old_pte.modified) {
            if (old_pte.file_mapped) {
                if (O_option) std::cout << " FOUT\n";
                old_process.pstats.fouts++;
                total_cost += COST_FOUT;
            } else {
                if (O_option) std::cout << " OUT\n";
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

    // Map new page
    frame->occupied = true;
    frame->pid = current_process_id;
    frame->vpage = vpage;

    PTE& pte = current_process->page_table[vpage];
    pte.present = 1;
    pte.frame = frame - &frame_table[0];

    // Set write_protect and file_mapped from VMA only on first page fault
    if (!pte.initialized) {
        VMA vma = current_process->getVMA(vpage);
        pte.write_protect = vma.write_protect;
        pte.file_mapped = vma.file_mapped;
        pte.initialized = 1;  // Mark as initialized
    }

    // Decide on IN/ZERO/FIN and print messages accordingly
    if (pte.file_mapped) {
        if (O_option) std::cout << " FIN\n";
        current_process->pstats.fins++;
        total_cost += COST_FIN;
    } else {
        if (pte.paged_out) {
            if (O_option) std::cout << " IN\n";
            current_process->pstats.ins++;
            total_cost += COST_IN;
        } else {
            if (O_option) std::cout << " ZERO\n";
            current_process->pstats.zeros++;
            total_cost += COST_ZERO;
        }
    }

    if (O_option) std::cout << " MAP " << pte.frame << "\n";
    current_process->pstats.maps++;
    total_cost += COST_MAP;

    // For Aging Pager, reset age counter
    if (auto agingPager = dynamic_cast<AgingPager*>(pager)) {
        size_t frame_idx = pte.frame;
        agingPager->resetAgeCounter(frame_idx);  // Use the public method
    }


    // For Working Set Pager, update last used time
    if (auto wsPager = dynamic_cast<WorkingSetPager*>(pager)) {
        size_t frame_idx = pte.frame;
        wsPager->updateLastUsedTime(frame_idx, inst_count);
    }
}

void MMU::handleProcessExit(int procid) {
    Process& proc = processes[procid];

    for (int i = 0; i < MAX_VPAGES; ++i) {
        PTE& pte = proc.page_table[i];
        if (pte.present) {
            FTE& frame = frame_table[pte.frame];
            if (O_option) std::cout << " UNMAP " << procid << ":" << i << "\n";
            proc.pstats.unmaps++;
            total_cost += COST_UNMAP;

            if (pte.modified) {
                if (pte.file_mapped) {
                    if (O_option) std::cout << " FOUT\n";
                    proc.pstats.fouts++;
                    total_cost += COST_FOUT;
                }
                // No OUT on process exit for anonymous pages
            }

            // Clear the frame and return to the free pool
            frame.occupied = false;
            frame.pid = -1;
            frame.vpage = -1;
            free_frames.push_back(&frame);

            pte.present = 0;
            pte.referenced = 0;
            pte.modified = 0;
            pte.frame = 0;
        }
        pte.paged_out = 0;
    }
    if (O_option) std::cout << " EXIT\n";
    process_exits++;
    total_cost += COST_PROCESS_EXIT;
}

FTE* MMU::get_frame() {
    // Check for free frame
    if (!free_frames.empty()) {
        FTE* frame = free_frames.front();
        free_frames.pop_front();
        return frame;
    }
    // Use the pager to select a victim frame
    FTE* frame = pager->select_victim_frame(frame_table);
    return frame;
}

void MMU::printPageTable() {
    if (P_option) {
        for (const auto& process : processes) {
            std::cout << "PT[" << process.pid << "]: ";
            for (int i = 0; i < MAX_VPAGES; ++i) {
                PTE pte = process.page_table[i];
                if (pte.present) {
                    std::cout << i << ":";
                    std::cout << (pte.referenced ? "R" : "-");
                    std::cout << (pte.modified ? "M" : "-");
                    std::cout << (pte.paged_out ? "S" : "-") << " ";
                } else {
                    if (pte.paged_out) {
                        std::cout << "# ";
                    } else {
                        std::cout << "* ";
                    }
                }
            }
            std::cout << "\n";
        }
    }
}

void MMU::printFrameTable() {
    if (F_option) {
        std::cout << "FT: ";
        for (size_t i = 0; i < frame_table.size(); ++i) {
            FTE& frame = frame_table[i];
            if (frame.occupied) {
                std::cout << frame.pid << ":" << frame.vpage << " ";
            } else {
                std::cout << "* ";
            }
        }
        std::cout << "\n";
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

// Implementations of Pager methods

// NRU Pager select_victim_frame
FTE* NRUPager::select_victim_frame(std::vector<FTE>& frame_table) {
    size_t num_frames = frame_table.size();
    bool reset_referenced = false;
    int lowest_class = 4;  // Initialize to an impossible class
    FTE* victim = nullptr;
    FTE* class_frames[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t frames_scanned = 0;

    // Check if we need to reset R bits
    if (instr_count - last_reset >= 50) {
        reset_referenced = true;
        last_reset = instr_count;
    }

    size_t start_hand = hand;

    do {
        FTE* frame = &frame_table[hand];
        Process& proc = (*processes)[frame->pid];
        PTE& pte = proc.page_table[frame->vpage];

        int class_idx = 0;
        if (pte.referenced) class_idx += 2;
        if (pte.modified) class_idx += 1;

        // Keep the first frame found in each class
        if (!class_frames[class_idx]) {
            class_frames[class_idx] = frame;
            if (class_idx < lowest_class) {
                lowest_class = class_idx;
            }
        }

        // If R bits need to be reset, reset them after considering the class
        if (reset_referenced) {
            pte.referenced = 0;
        }

        hand = (hand + 1) % num_frames;
        frames_scanned++;

        // Stop scanning if we find a Class 0 frame and R bits are not being reset
        if (!reset_referenced && class_idx == 0) {
            break;
        }
    } while (hand != start_hand);

    victim = class_frames[lowest_class];

    // Update hand to the frame after the victim
    hand = (victim - &frame_table[0] + 1) % num_frames;

    // Print debug info if 'a' option is enabled
    if (a_option) {
        std::cout << "ASELECT: " << start_hand << " " << (reset_referenced ? 1 : 0) << " | "
                  << lowest_class << " " << (victim - &frame_table[0]) << " " << frames_scanned << "\n";
    }

    return victim;
}

// Aging Pager select_victim_frame
FTE* AgingPager::select_victim_frame(std::vector<FTE>& frame_table) {
    size_t num_frames = frame_table.size();
    uint32_t min_age = UINT32_MAX;
    FTE* victim = nullptr;
    size_t start_hand = hand;

    if (a_option) {
        std::cout << "ASELECT " << hand << "-" << ((hand + num_frames - 1) % num_frames) << " | ";
    }

    for (size_t i = 0; i < num_frames; ++i) {
        FTE* frame = &frame_table[hand];
        Process& proc = (*processes)[frame->pid];
        PTE& pte = proc.page_table[frame->vpage];

        // Shift right and set the leftmost bit if referenced
        age_counters[hand] >>= 1;
        if (pte.referenced) {
            age_counters[hand] |= 0x80000000;  // Set leftmost bit
            pte.referenced = 0;                // Reset referenced bit
        }

        if (a_option) {
            std::cout << hand << ":" << std::hex << age_counters[hand] << " ";
        }

        // Keep track of frame with minimum age
        if (age_counters[hand] < min_age) {
            min_age = age_counters[hand];
            victim = frame;
        }

        hand = (hand + 1) % num_frames;
    }

    // Update hand to the frame after the victim
    hand = (victim - &frame_table[0] + 1) % num_frames;

    if (a_option) {
        std::cout << "| " << (victim - &frame_table[0]) << "\n";
    }

    return victim;
}

// Working Set Pager select_victim_frame
FTE* WorkingSetPager::select_victim_frame(std::vector<FTE>& frame_table) {
    size_t num_frames = frame_table.size();
    unsigned long long oldest_time = instr_count;
    FTE* victim = nullptr;
    size_t frames_scanned = 0;
    size_t start_hand = hand;
    const int TAU = 50;

    if (a_option) {
        std::cout << "ASELECT " << hand << "-" << ((hand + num_frames - 1) % num_frames) << " | ";
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
                // Found victim
                victim = frame;
                if (a_option) {
                    std::cout << hand << "(" << pte.referenced << " " << frame->pid << ":" << frame->vpage << " "
                              << last_used[hand] << ") ";
                }
                break;
            }
        }

        // Keep track of oldest frame if no victim is found
        if (!victim || last_used[hand] < oldest_time) {
            oldest_time = last_used[hand];
            victim = frame;
        }

        if (a_option) {
            std::cout << hand << "(" << pte.referenced << " " << frame->pid << ":" << frame->vpage << " "
                      << last_used[hand] << ") ";
        }

        hand = (hand + 1) % num_frames;
    } while (hand != start_hand);

    if (a_option) {
        std::cout << "| " << (victim - &frame_table[0]) << "\n";
    }

    // Update hand to the frame after the victim
    hand = (victim - &frame_table[0] + 1) % num_frames;

    return victim;
}

int main(int argc, char* argv[]) {
    int num_frames = 128;
    Pager* pager = nullptr;
    std::string algo = "f";
    std::string options;
    std::string inputfile;
    std::string randomfile;

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
                std::cerr << "Unknown option " << c << "\n";
                return 1;
        }
    }

    if (optind + 2 > argc) {
        std::cerr << "Error: Missing inputfile and/or randomfile\n";
        return 1;
    }

    inputfile = argv[optind];
    randomfile = argv[optind + 1];

    RandomNumberGenerator randGen(randomfile);

    // Initialize MMU without a pager
    MMU mmu(num_frames, nullptr);
    mmu.setOptions(options);
    mmu.loadInput(inputfile);

    // Check if '-a' option is provided in 'options'
    bool a_option = false;
    for (char ch : options) {
        if (ch == 'a') {
            a_option = true;
            break;
        }
    }

    // Initialize pager after MMU to access processes if needed
    if (algo == "f") {
        pager = new FIFOPager();
    } else if (algo == "r") {
        pager = new RandomPager(&randGen, num_frames);
    } else if (algo == "c") {
        ClockPager* clockPager = new ClockPager();
        pager = clockPager;
        clockPager->setProcesses(&mmu.getProcesses());
    } else if (algo == "e") {
        NRUPager* nruPager = new NRUPager(&mmu.getProcesses());
        nruPager->setOptions(a_option);
        pager = nruPager;
    } else if (algo == "a") {
        AgingPager* agingPager = new AgingPager(&mmu.getProcesses(), num_frames);
        agingPager->setOptions(a_option);
        pager = agingPager;
    } else if (algo == "w") {
        WorkingSetPager* wsPager = new WorkingSetPager(&mmu.getProcesses(), num_frames);
        wsPager->setOptions(a_option);
        pager = wsPager;
    } else {
        std::cerr << "Unknown algorithm '" << algo << "'\n";
        return 1;
    }

    // Set the pager in MMU using the public setter
    mmu.setPager(pager);

    mmu.simulate();
    mmu.printPageTable();
    mmu.printFrameTable();
    mmu.printSummary();

    delete pager;
    return 0;
}