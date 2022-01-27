#include <verilated.h>
#include "Vemu.h"

#include "imgui.h"
#ifndef _MSC_VER
#include <stdio.h>
#include <SDL.h>
#include <SDL_opengl.h>
#else
#define WIN32
#include <dinput.h>
#endif

#include "sim_console.h"
#include "sim_bus.h"
#include "sim_video.h"
#include "sim_input.h"
#include "sim_clock.h"

#include "../imgui/imgui_memory_editor.h"
#include "../imgui/ImGuiFileDialog.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <iterator>
#include <string>
#include <sstream>
#include <iomanip>

using namespace std;

// Simulation control
// ------------------
int initialReset = 48;
bool run_enable = 1;
int batchSize = 25000000 / 100000;
bool single_step = 0;
bool multi_step = 0;
int multi_step_amount = 1024;

// Debug GUI 
// ---------
const char* windowTitle = "Verilator Sim: Aznable";
const char* windowTitle_Control = "Simulation control";
const char* windowTitle_DebugLog = "Debug log";
const char* windowTitle_Video = "VGA output";
bool showDebugLog = true;
DebugConsole console;
MemoryEditor mem_edit;

// HPS emulator
// ------------
SimBus bus(console);

// Input handling
// --------------
SimInput input(12, console);
const int input_right = 0;
const int input_left = 1;
const int input_down = 2;
const int input_up = 3;
const int input_fire1 = 4;
const int input_fire2 = 5;
const int input_start_1 = 6;
const int input_start_2 = 7;
const int input_coin_1 = 8;
const int input_coin_2 = 9;
const int input_coin_3 = 10;
const int input_pause = 11;

// Video
// -----
#define VGA_WIDTH 256
#define VGA_HEIGHT 225
#define VGA_ROTATE 0  // 90 degrees anti-clockwise
#define VGA_SCALE_X vga_scale
#define VGA_SCALE_Y vga_scale
SimVideo video(VGA_WIDTH, VGA_HEIGHT, VGA_ROTATE);
float vga_scale = 3.0;

// Verilog module
// --------------
Vemu* top = NULL;
vluint64_t main_time = 0; // Current simulation time.
double sc_time_stamp()
{ // Called by $time in Verilog.
	return main_time;
}

int clockSpeed = 8;	 // This is not used, just a reminder for the dividers below
SimClock clk_sys(1); // 4mhz

void resetSim()
{
	main_time = 0;
	top->reset = 1;
	clk_sys.Reset();
}

// MAME debug log

bool log_instructions = true;
bool stop_on_log_mismatch = true;
bool break_vbl = 1;

std::vector<std::string> log_mame;
std::vector<std::string> log_cpu;
long log_index;
unsigned int ins_count = 0;


// CPU debug
bool cpu_sync;
bool cpu_sync_last;
std::vector<std::vector<std::string> > opcodes;
std::map<std::string, std::string> opcode_lookup;


bool writeLog(const char* line)
{
	// Write to cpu log
	log_cpu.push_back(line);

	// Compare with MAME log
	bool match = true;
	ins_count++;

	std::string c_line = std::string(line);

	std::string c = "%d > " + c_line;

	if (log_index < log_mame.size()) {
		std::string m_line = log_mame.at(log_index);
		//std::string f = fmt::format("{0}: hcnt={1} vcnt={2} {3} {4}", log_index, top->top__DOT__missile__DOT__sc__DOT__hcnt, top->top__DOT__missile__DOT__sc__DOT__vcnt, m, c);
		//console.AddLog(f.c_str());
		if (log_instructions) { console.AddLog(c.c_str(), ins_count); }

		if (stop_on_log_mismatch && m_line != c_line) {
			console.AddLog("DIFF at %d", log_index);
			std::string m = "MAME > " + m_line;
			console.AddLog(m.c_str());
			console.AddLog(c.c_str(), ins_count);
			match = false;
			run_enable = 0;
		}
	}
	else {
		console.AddLog("MAME OUT");
		run_enable = 0;
	}

	log_index++;
	return match;

}

void loadOpcodes()
{
	std::string fileName = "8080_opcodes.csv";

	std::string                           header;
	std::ifstream                         reader(fileName);
	if (reader.is_open()) {
		std::string line, column, id;
		std::getline(reader, line);
		header = line;
		while (std::getline(reader, line)) {
			std::stringstream        ss(line);
			std::vector<std::string> columns;
			bool                     withQ = false;
			std::string              part{ "" };
			while (std::getline(ss, column, ',')) {
				auto pos = column.find("\"");
				if (pos < column.length()) {
					withQ = !withQ;
					part += column.substr(0, pos);
					column = column.substr(pos + 1, column.length());
				}
				if (!withQ) {
					column += part;
					columns.emplace_back(std::move(column));
					part = "";
				}
				else {
					part += column + ",";
				}
			}
			opcodes.push_back(columns);
			opcode_lookup[columns[0]] = columns[1];
		}
	}
};

std::string int_to_hex(unsigned char val)
{
	std::stringstream ss;
	ss << std::setfill('0') << std::setw(2) << std::hex << (val | 0);
	return ss.str();
}

std::string get_opcode(int i)
{
	std::string hex = "0x";
	hex.append(int_to_hex(i));
	std::string code = opcode_lookup[hex];
	return code;
}

bool hasEnding(std::string const& fullString, std::string const& ending) {
	if (fullString.length() >= ending.length()) {
		return (0 == fullString.compare(fullString.length() - ending.length(), ending.length(), ending));
	}
	else {
		return false;
	}
}

std::string last_log;

unsigned short active_pc;
unsigned short last_pc;

bool new_ins_last;

const int ins_size = 48;
int ins_index = 0;
int ins_pc[ins_size];
int ins_in[ins_size];
int ins_ma[ins_size];
unsigned char active_ins = 0;

bool vbl_last;

int verilate()
{

	if (!Verilated::gotFinish())
	{

		// Assert reset during startup
		if (main_time < initialReset) { top->reset = 1; }
		// Deassert reset after startup
		if (main_time == initialReset) { top->reset = 0; }

		// Clock dividers
		clk_sys.Tick();

		// Set system clock in core
		top->clk_sys = clk_sys.clk;

		//// Update console with current cycle for logging
		//console.prefix = "(" + std::to_string(main_time) + ") ";

		// Simulate both edges of system clock
		if (clk_sys.clk != clk_sys.old) {
			if (clk_sys.clk) {
				input.BeforeEval();
				bus.BeforeEval();
			}
			top->eval();
			if (clk_sys.clk) { bus.AfterEval(); }

			if (!top->reset ) {
				cpu_sync = top->emu__DOT__blockade__DOT__SYNC;

				//bool vbl = top->VGA_VB;
				//if (vbl && !vbl_last)
				//{
				//	if (break_vbl) {
				//		run_enable = 0;
				//	}
				//	console.AddLog("VBL");
				//	console.AddLog("A=%02x", top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__acc);
				//	console.AddLog("BC=%04x", top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__r16_bc);
				//	console.AddLog("DE=%04x", top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__r16_de);
				//	console.AddLog("HL=%04x", top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__r16_hl);
				//}
				//vbl_last = vbl;

				unsigned short pc = top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__r16_pc;
				unsigned char di = top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__di;
				unsigned short ad = top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__a;
				unsigned char i = top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__i;

				bool pin_f1 = top->emu__DOT__blockade__DOT__cpu__DOT__f1_core;
				bool pin_f2 = top->emu__DOT__blockade__DOT__cpu__DOT__f2_core;
				bool pin_m1 = top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__m1;
				bool pin_t3 = top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__t3;

				if (pc != last_pc) {
					//console.AddLog("%08d PC> PC=%04x I=%02x AD=%04x DI=%02x  PC0=%04x sync=%x f1=%x f2=%x", main_time, pc, i, ad, di, ins_pc[0], cpu_sync, pin_f1, pin_f2);
				}
				last_pc = pc;

				bool new_ins = !pin_f2 && pin_f1 && (pin_m1 && pin_t3);
				if (new_ins && !new_ins_last) {

					if (active_ins != 0)
					{
						unsigned char skip = (ins_count == 0) ? 2 : 1;
						unsigned char data1 = ins_in[skip];
						unsigned char data2 = ins_in[skip + 1];

						std::string fmt = "%04X: ";
						std::string opcode = get_opcode(active_ins);
						if (hasEnding(opcode, "d16") || hasEnding(opcode, "adr")) {
							opcode.resize(opcode.length() - 3);
							char buf[6];
							sprintf(buf, "$%02x%02x", data2, data1);
							opcode.append(buf);
						}
						if (hasEnding(opcode, "d8")) {
							opcode.resize(opcode.length() - 2);
							char buf[6];
							sprintf(buf, "$%02x", data1);
							opcode.append(buf);
						}
						fmt.append(opcode);

						unsigned short acc = top->emu__DOT__blockade__DOT__cpu__DOT__core__DOT__acc;
						char buf[1024];
						sprintf(buf, fmt.c_str(), active_pc);
						writeLog(buf);
						// Clear instruction cache
						ins_index = 0;
						for (int i = 0; i < ins_size; i++) {
							ins_in[i] = 0;
							ins_ma[i] = 0;
						}
					}
					active_ins = i;
					active_pc = ad;
				}
				new_ins_last = new_ins;

				if (cpu_sync && !cpu_sync_last) {
					//console.AddLog("%08d SC> PC=%04x I=%02x AD=%04x DI=%02x   PC0=%04x", main_time, pc, i, ad, di, ins_pc[0]);
					ins_pc[ins_index] = pc;
					ins_in[ins_index] = di;
					ins_ma[ins_index] = ad;
					ins_index++;
					if (ins_index > ins_size - 1) { ins_index = 0; }
				}


				cpu_sync_last = cpu_sync;
			}

		}

#ifdef DEBUG_AUDIO
		clk_audio.Tick();
		if (clk_audio.IsRising()) {
			// Output audio
			unsigned short audio_l = top->AUDIO_L;
			audioFile.write((const char*)&audio_l, 2);
		}
#endif

		// Output pixels on rising edge of pixel clock
		if (clk_sys.IsRising() && top->emu__DOT__ce_pix) {
			uint32_t colour = 0xFF000000 | top->VGA_B << 16 | top->VGA_G << 8 | top->VGA_R;
			video.Clock(top->VGA_HB, top->VGA_VB, top->VGA_HS, top->VGA_VS, colour);
		}

		main_time++;
		return 1;
	}
	// Stop verilating and cleanup
	top->final();
	delete top;
	exit(0);
	return 0;
}

int main(int argc, char** argv, char** env)
{
	// Create core and initialise
	top = new Vemu();
	Verilated::commandArgs(argc, argv);

#ifdef WIN32
	// Attach debug console to the verilated code
	Verilated::setDebug(console);
#endif

	// Load debug opcodes
	loadOpcodes();

	// Load debug trace
	std::string line;
	std::ifstream fin("blockade.tr");
	while (getline(fin, line)) {
		log_mame.push_back(line);
	}

	// Attach bus
	bus.ioctl_addr = &top->ioctl_addr;
	bus.ioctl_index = &top->ioctl_index;
	bus.ioctl_wait = &top->ioctl_wait;
	bus.ioctl_download = &top->ioctl_download;
	bus.ioctl_upload = &top->ioctl_upload;
	bus.ioctl_wr = &top->ioctl_wr;
	bus.ioctl_dout = &top->ioctl_dout;
	bus.ioctl_din = &top->ioctl_din;

	// Set up input module
	input.Initialise();
#ifdef WIN32
	input.SetMapping(input_up, DIK_UP);
	input.SetMapping(input_right, DIK_RIGHT);
	input.SetMapping(input_down, DIK_DOWN);
	input.SetMapping(input_left, DIK_LEFT);
	input.SetMapping(input_fire1, DIK_LCONTROL);
	input.SetMapping(input_fire2, DIK_LALT);
	input.SetMapping(input_start_1, DIK_1);
	input.SetMapping(input_coin_1, DIK_5);
#else
	input.SetMapping(input_up, SDL_SCANCODE_UP);
	input.SetMapping(input_right, SDL_SCANCODE_RIGHT);
	input.SetMapping(input_down, SDL_SCANCODE_DOWN);
	input.SetMapping(input_left, SDL_SCANCODE_LEFT);
	input.SetMapping(input_fire1, SDL_SCANCODE_LCONTROL);
	input.SetMapping(input_fire2, SDL_SCANCODE_LALT);
	input.SetMapping(input_start_1, SDL_SCANCODE_1);
	input.SetMapping(input_coin_1, SDL_SCANCODE_3);
#endif
	// Setup video output
	if (video.Initialise(windowTitle) == 1)
	{
		return 1;
	}

	// Reset simulation
	resetSim();

	// Stage roms for this core
	bus.QueueDownload("roms/blockade/316-0001.u43", 0);
	bus.QueueDownload("roms/blockade/316-0002.u29", 0);
	bus.QueueDownload("roms/blockade/316-0003.u3", 0);
	bus.QueueDownload("roms/blockade/316-0004.u2", 0);

#ifdef WIN32
	MSG msg;
	ZeroMemory(&msg, sizeof(msg));
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			continue;
		}
#else
	bool done = false;
	while (!done)
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			ImGui_ImplSDL2_ProcessEvent(&event);
			if (event.type == SDL_QUIT)
				done = true;
		}
#endif
		video.StartFrame();

		input.Read();

		// Draw GUI
		// --------
		ImGui::NewFrame();

		// Simulation control window
		ImGui::Begin(windowTitle_Control);
		ImGui::SetWindowPos(windowTitle_Control, ImVec2(0, 0), ImGuiCond_Once);
		ImGui::SetWindowSize(windowTitle_Control, ImVec2(500, 150), ImGuiCond_Once);
		if (ImGui::Button("Reset simulation")) { resetSim(); } ImGui::SameLine();
		if (ImGui::Button("Start running")) { run_enable = 1; } ImGui::SameLine();
		if (ImGui::Button("Stop running")) { run_enable = 0; } ImGui::SameLine();
		ImGui::Checkbox("RUN", &run_enable);
		//ImGui::PopItemWidth();
		ImGui::SliderInt("Run batch size", &batchSize, 1, 250000);
		if (single_step == 1) { single_step = 0; }
		if (ImGui::Button("Single Step")) { run_enable = 0; single_step = 1; }
		ImGui::SameLine();
		if (multi_step == 1) { multi_step = 0; }
		if (ImGui::Button("Multi Step")) { run_enable = 0; multi_step = 1; }
		//ImGui::SameLine();
		ImGui::SliderInt("Multi step amount", &multi_step_amount, 8, 1024);

		ImGui::NewLine();

		ImGui::Checkbox("Log CPU instructions", &log_instructions);
		ImGui::Checkbox("Stop on MAME diff", &stop_on_log_mismatch);
		

		ImGui::End();

		// Debug log window
		console.Draw(windowTitle_DebugLog, &showDebugLog, ImVec2(500, 700));
		ImGui::SetWindowPos(windowTitle_DebugLog, ImVec2(0, 160), ImGuiCond_Once);
		// Video window
		ImGui::Begin(windowTitle_Video);
		ImGui::SetWindowPos(windowTitle_Video, ImVec2(550, 0), ImGuiCond_Once);
		ImGui::SetWindowSize(windowTitle_Video, ImVec2((VGA_WIDTH * VGA_SCALE_X) + 24, (VGA_HEIGHT * VGA_SCALE_Y) + 130), ImGuiCond_Once);

		ImGui::SliderFloat("Zoom", &vga_scale, 1, 8);
		ImGui::SliderInt("Rotate", &video.output_rotate, -1, 1); ImGui::SameLine();
		ImGui::Checkbox("Flip V", &video.output_vflip);
		ImGui::Text("main_time: %d frame_count: %d sim FPS: %f", main_time, video.count_frame, video.stats_fps);
		ImGui::Text("pixel: %06d line: %03d", video.count_pixel, video.count_line);
		ImGui::Text("xmax: %04d ymax: %04d", video.stats_xMax, video.stats_yMax);

		//float vol_l = ((signed short)(top->AUDIO_L) / 256.0f) / 256.0f;
		//float vol_r = ((signed short)(top->AUDIO_R) / 256.0f) / 256.0f;
		//ImGui::ProgressBar(vol_l + 0.5, ImVec2(200, 16), 0); ImGui::SameLine();
		//ImGui::ProgressBar(vol_r + 0.5, ImVec2(200, 16), 0);

		// Draw VGA output
		ImGui::Image(video.texture_id, ImVec2(video.output_width * VGA_SCALE_X, video.output_height * VGA_SCALE_Y));
		ImGui::End();

		//ImGui::Begin("rom_lsb");
		//mem_edit.DrawContents(&top->emu__DOT__blockade__DOT__rom_lsb__DOT__mem, 1024, 0);
		//ImGui::End();
		//ImGui::Begin("rom_msb");
		//mem_edit.DrawContents(&top->emu__DOT__blockade__DOT__rom_msb__DOT__mem, 1024, 0);
		//ImGui::End();
		//ImGui::Begin("ram");
		//mem_edit.DrawContents(&top->emu__DOT__blockade__DOT__ram__DOT__mem, 1024, 0);
		//ImGui::End();
		//ImGui::Begin("sram");
		//mem_edit.DrawContents(&top->emu__DOT__blockade__DOT__sram__DOT__mem, 256, 0);
		//ImGui::End();
		//ImGui::Begin("prom_lsb");
		//mem_edit.DrawContents(&top->emu__DOT__blockade__DOT__rom_lsb__DOT__mem, 512, 0);
		//ImGui::End();
		//ImGui::Begin("prom_msb");
		//mem_edit.DrawContents(&top->emu__DOT__blockade__DOT__rom_msb__DOT__mem, 512, 0);
		//ImGui::End();

		// ImGui::Begin("ROM");
		// memoryEditor_hs.DrawContents(&top->top__DOT__blockade__DOT__MEM_ROM__DOT__ram, 4096, 0);
		// ImGui::End();

		video.UpdateTexture();

		// Pass inputs to sim
		top->inputs = 0;
		for (int i = 0; i < input.inputCount; i++)
		{
			if (input.inputs[i])
			{
				top->inputs |= (1 << i);
			}
		}

		top->IN0 = 255 ^ ((input.inputs[input_start_1]) << 7);
		top->IN1 = 0b11111111;
		top->IN2 = 255;

		// Run simulation
		if (run_enable)
		{
			for (int step = 0; step < batchSize; step++)
			{
				verilate();
			}
		}
		else
		{
			if (single_step)
			{
				verilate();
			}
			if (multi_step)
			{
				for (int step = 0; step < multi_step_amount; step++)
				{
					verilate();
				}
			}
		}
	}

	// Clean up before exit
	// --------------------

	video.CleanUp();
	input.CleanUp();

	return 0;
}
