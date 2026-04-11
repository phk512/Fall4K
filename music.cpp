#define NOMINMAX // 防止 windows.h 的 min/max 宏与 std::min/max 冲突
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <cmath>
#include <algorithm>
#include <ctime>
#include <windows.h>

// ================= 基础配置 =================
const int SCREEN_WIDTH = 80;
const int SCREEN_HEIGHT = 40;
const int LANE_WIDTH = 6;
// 【修改】将常量改为基础常量，便于异象计算动态偏移
const int BASE_LANE_START_X = (SCREEN_WIDTH - (LANE_WIDTH * 4 + 5)) / 2;
const int BASE_JUDGE_LINE_Y = SCREEN_HEIGHT - 6;
const int KEYS[4] = { 'D', 'F', 'J', 'K' };

// 全局动态坐标
int currentLaneStartX = BASE_LANE_START_X;
int currentJudgeLineY = BASE_JUDGE_LINE_Y;

// ================= 数据结构 =================
enum NoteType { TAP, HOLD };

struct Note {
	long long hitTime;
	int lane;
	NoteType type;
	long long duration;
	bool processed = false;
	bool isBeingHeld = false;
};

// 【修改】全面升级异象结构体，支持多种类型
enum AnomalyType { BOOM, X_SHIFT, Y_SHIFT, RAIN, FLASH };
struct Anomaly {
	AnomalyType type;
	long long startTime;
	long long endTime;
	float val; // 用于 x 和 y 的偏移量
};

struct Toast {
	std::string text;
	int yOffset;
	long long startTime;
	long long endTime;
};

struct Particle {
	float x, y;
	float vx, vy;
	int life, maxLife;
	char symbol;
	WORD color;
};

// 【新增】设置项数据结构
struct GameSettings {
	bool enableStarfield = true;
	bool enableKeyEffects = true;
	int hiddenLevel = 0;      // 上隐行数，范围 0~34
	int audioOffset = 0;      // 延迟补偿，范围 -300 ~ 300 ms
	bool beginnerMode = false;// 新手模式
	bool enableTapSound = true; // 打击音效开关，默认开启
	bool showRealTimeAcc = true; // 实时ACC开关
	bool autoPlay = false;       // 全自动模式开关
};

GameSettings settings; // 全局设置对象

// ================= 动态加载音频函数 =================
typedef MCIERROR(WINAPI* PFN_mciSendStringA)(LPCSTR, LPSTR, UINT, HWND);
PFN_mciSendStringA dynMciSendStringA = nullptr;

bool initAudioSystem() {
	HMODULE hWinmm = LoadLibraryA("winmm.dll");
	if (!hWinmm) return false;
	dynMciSendStringA = (PFN_mciSendStringA)GetProcAddress(hWinmm, "mciSendStringA");
	
	if (dynMciSendStringA) {
		// 预加载打击音效，放置在 sound 文件夹中
		dynMciSendStringA("open \"sound/tap.mp3\" type mpegvideo alias hit_sound", NULL, 0, NULL);
	}
	
	return dynMciSendStringA != nullptr;
}

void playTapSound() {
	if (dynMciSendStringA && settings.enableTapSound) {
		// "from 0" 很重要，确保每次点击都从头开始播放，覆盖之前的声音
		dynMciSendStringA("play hit_sound from 0", NULL, 0, NULL);
	}
}
void sendAudioCommand(const std::string& cmd) {
	if (dynMciSendStringA) {
		dynMciSendStringA(cmd.c_str(), NULL, 0, NULL);
	}
}

// ================= 全局游戏状态 =================
std::vector<Note> notes;
std::vector<Anomaly> anomalies;
std::vector<Toast> toasts;
std::vector<Particle> particles;

long long gameStartTime = 0;
long long globalStateEnterTime = 0; // 【特效专用】全局UI状态进入时间
int combo = 0;
int maxCombo = 0;
std::string lastJudge = "READY";
int judgeDisplayTimer = 0;
long long lastHitTime = 0;

int statPerfect = 0;
int statGreat = 0;
int statBad = 0;
int statMiss = 0;

long long hitShakeEndTime = 0;
float hitShakeIntensity = 0.0f;

bool prevKeyState[4] = { false, false, false, false };
bool prevEnterState = false;

WORD laneColors[4] = { FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED, FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED, FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED, FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED };
long long laneEffectEndTime[4] = { 0, 0, 0, 0 };

std::string musicFileName = "music.mp3";
std::string currentChartFile = "chart.txt";
long long chartTotalDuration = 0;
float chartSpeed = 0.15f;

enum GameState { STATE_MENU, STATE_SELECT_CHART, STATE_PLAYING, STATE_REC_SETUP, STATE_RECORDING, STATE_RESULTS, STATE_SETTINGS, STATE_EXIT };
GameState currentState = STATE_MENU;
int selectedMenuItem = 0;
int selectedSettingItem = 0; // 设置菜单选项索引

const int MENU_ITEM_COUNT = 5;
// 严格保留原本文案
std::string menuItems[MENU_ITEM_COUNT] = { "1. 开始游戏", "2. 游戏教学", "3. 谱面录制", "4. 游戏设置", "5. 离开" };

std::vector<std::string> chartFiles;
int selectedChartIndex = 0;

HANDLE hConsole;
CHAR_INFO playfieldLayer[SCREEN_HEIGHT][SCREEN_WIDTH];
CHAR_INFO finalLayer[SCREEN_HEIGHT][SCREEN_WIDTH];

// ================= 工具与文件函数 =================

long long parseTime(const std::string& timeStr) {
	int min, sec, ms;
	char dot1, dot2;
	std::stringstream ss(timeStr);
	ss >> min >> dot1 >> sec >> dot2 >> ms;
	return min * 60000LL + sec * 1000LL + ms;
}

std::string formatTime(long long ms) {
	long long min = ms / 60000;
	long long sec = (ms % 60000) / 1000;
	long long msec = ms % 1000;
	char buf[32];
	sprintf_s(buf, "%lld.%02lld.%03lld", min, sec, msec);
	return std::string(buf);
}

// 保存/加载设置系统
void loadSettings() {
	std::ifstream file("setting.log");
	if (!file.is_open()) return;
	std::string key;
	while (file >> key) {
		if (key == "starfield") file >> settings.enableStarfield;
		else if (key == "keyeffects") file >> settings.enableKeyEffects;
		else if (key == "hidden") file >> settings.hiddenLevel;
		else if (key == "offset") file >> settings.audioOffset;
		else if (key == "beginner") file >> settings.beginnerMode;
		else if (key == "tapsound") file >> settings.enableTapSound;
		else if (key == "realtimeacc") file >> settings.showRealTimeAcc;
		else if (key == "autoplay") file >> settings.autoPlay;
	}
	file.close();
}

void saveSettings() {
	std::ofstream file("setting.log");
	file << "starfield " << settings.enableStarfield << "\n";
	file << "keyeffects " << settings.enableKeyEffects << "\n";
	file << "hidden " << settings.hiddenLevel << "\n";
	file << "offset " << settings.audioOffset << "\n";
	file << "beginner " << settings.beginnerMode << "\n";
	file << "tapsound " << settings.enableTapSound << "\n";
	file << "realtimeacc " << settings.showRealTimeAcc << "\n";
	file << "autoplay " << settings.autoPlay << "\n";
	file.close();
}

void spawnParticles(int lane, WORD color, int count, float spread = 1.0f) {
	if (!settings.enableKeyEffects) return; 
	for (int p = 0; p < count; ++p) {
		Particle part;
		// 动态获取当前的横纵坐标
		part.x = currentLaneStartX + lane * LANE_WIDTH + 3;
		part.y = currentJudgeLineY;
		part.vx = ((rand() % 100) / 50.0f - 1.0f) * 1.5f * spread;
		part.vy = -((rand() % 100) / 50.0f) * 1.2f - 0.5f;
		part.life = part.maxLife = 10 + rand() % 15;
		part.symbol = ".*+`'"[rand() % 5];
		part.color = color;
		particles.push_back(part);
	}
}

void createDefaultChart() {
	std::ofstream file("chart/chart.txt");
	file << "AUDIO music.mp3\nTIME 0.08.000\nSPEED 0.18\n";
	file << "0.0.500 1 tap 0\n0.1.000 2 tap 0\n0.1.500 3 tap 0\n0.2.000 4 tap 0\n0.2.500 2 hold 2000\n";
	file << "0.3.000 boom 0.3.500\n0.4.000 text 你好 5 0.6.000\n0.5.000 3 hold 1500\n0.5.500 boom 0.7.500\n0.7.000 1 tap 0\n0.7.500 4 tap 0\n";
	file.close();
}

void loadChart(const std::string& filename) {
	notes.clear(); anomalies.clear(); toasts.clear(); particles.clear();
	std::ifstream file("chart/" + filename);
	if (!file.is_open()) { createDefaultChart(); file.open("chart/chart.txt"); }
	
	std::string line, key;
	if (std::getline(file, line)) { std::stringstream ss(line); ss >> key >> musicFileName; }
	if (std::getline(file, line)) { std::stringstream ss(line); std::string timeStr; ss >> key >> timeStr; chartTotalDuration = parseTime(timeStr); }
	if (std::getline(file, line)) { std::stringstream ss(line); ss >> key >> chartSpeed; }
	
	while (std::getline(file, line)) {
		if (line.empty()) continue;
		std::stringstream ss(line);
		std::string timeStr, typeStr; ss >> timeStr >> typeStr;
		
		// 【修改】解析新增的异象
		if (typeStr == "boom" || typeStr == "rain" || typeStr == "flash") {
			Anomaly a; 
			if (typeStr == "boom") a.type = BOOM;
			else if (typeStr == "rain") a.type = RAIN;
			else if (typeStr == "flash") a.type = FLASH;
			
			a.startTime = parseTime(timeStr);
			std::string endTimeStr; ss >> endTimeStr; 
			a.endTime = parseTime(endTimeStr);
			anomalies.push_back(a);
		} else if (typeStr == "x" || typeStr == "y") {
			Anomaly a;
			a.type = (typeStr == "x") ? X_SHIFT : Y_SHIFT;
			a.startTime = parseTime(timeStr);
			long long duration;
			ss >> a.val >> duration;
			a.endTime = a.startTime + duration;
			anomalies.push_back(a);
		} else if (typeStr == "text") {
			Toast t; t.startTime = parseTime(timeStr);
			ss >> t.text >> t.yOffset; std::string endTimeStr; ss >> endTimeStr; t.endTime = parseTime(endTimeStr);
			toasts.push_back(t);
		} else {
			Note n; n.hitTime = parseTime(timeStr); n.lane = std::stoi(typeStr) - 1;
			std::string noteType; ss >> noteType >> n.duration; n.type = (noteType == "hold") ? HOLD : TAP;
			notes.push_back(n);
		}
	}
}

long long getCurrentTimeMs() {
	auto now = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
	return duration.count() - gameStartTime - settings.audioOffset;
}

std::vector<std::string> scanChartFiles() {
	std::vector<std::string> files;
	WIN32_FIND_DATAA findData;
	// 改为扫描 chart 文件夹
	HANDLE hFind = FindFirstFileA("chart/*.txt", &findData);
	if (hFind != INVALID_HANDLE_VALUE) { do { files.push_back(findData.cFileName); } while (FindNextFileA(hFind, &findData)); FindClose(hFind); }
	return files;
}

void startReadyGo() {
	for (int i = 0; i < 3; ++i) { Beep(600, 200); std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
	Beep(900, 300); std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// ================= 渲染系统 =================

void clearBuffer(CHAR_INFO buffer[][SCREEN_WIDTH]) {
	for (int y = 0; y < SCREEN_HEIGHT; ++y) {
		for (int x = 0; x < SCREEN_WIDTH; ++x) {
			buffer[y][x].Char.AsciiChar = ' ';
			buffer[y][x].Attributes = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		}
	}
}

void drawString(CHAR_INFO buffer[][SCREEN_WIDTH], int x, int y, const std::string& text, WORD color = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED) {
	for (size_t i = 0; i < text.length(); ++i) {
		if (x + i >= 0 && x + i < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
			buffer[y][x + i].Char.AsciiChar = text[i];
			buffer[y][x + i].Attributes = color;
		}
	}
}

// 【特效：开场终端动画】
void playBootAnimation() {
	long long start = GetTickCount64();
	while(true) {
		long long t = GetTickCount64() - start;
		// 允许按回车跳过或播放2.6秒
		if (t > 2600 || (GetAsyncKeyState(VK_RETURN) & 0x8000)) break; 
		
		clearBuffer(finalLayer);
		for(int y = 0; y < SCREEN_HEIGHT; ++y) {
			if (rand() % 100 < (3000 - t) / 30) { 
				for(int x = 0; x < SCREEN_WIDTH; ++x) {
					if (rand() % 8 == 0) {
						finalLayer[y][x].Char.AsciiChar = rand() % 2 ? '0' : '1';
						finalLayer[y][x].Attributes = FOREGROUND_GREEN | (rand() % 2 ? FOREGROUND_INTENSITY : 0);
					}
				}
			}
		}
		
		if (t > 200) drawString(finalLayer, 2, 2, "Fall4K Engine Boot Sequence...", FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		if (t > 600) drawString(finalLayer, 2, 4, "> INITIALIZING AUDIO SUBSYSTEM... [ OK ]", FOREGROUND_GREEN);
		if (t > 1000) drawString(finalLayer, 2, 5, "> MOUNTING VIRTUAL FILE SYSTEM... [ OK ]", FOREGROUND_GREEN);
		if (t > 1400) drawString(finalLayer, 2, 6, "> LOADING CHART DECODERS...       [ OK ]", FOREGROUND_GREEN);
		if (t > 1800) drawString(finalLayer, 2, 7, "> ESTABLISHING NEURAL LINK...     [ OK ]", FOREGROUND_GREEN);
		if (t > 2200) drawString(finalLayer, 2, 9, "SYSTEM READY. ENTERING MAIN...", FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		
		COORD bufferSize = { SCREEN_WIDTH, SCREEN_HEIGHT }; COORD bufferCoord = { 0, 0 }; SMALL_RECT writeRegion = { 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1 };
		WriteConsoleOutput(hConsole, (CHAR_INFO*)finalLayer, bufferSize, bufferCoord, &writeRegion);
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
}

// 【修改】添加各种新特效参数支持
void renderGame(long long curTime, bool isBoom, float boomIntensity, bool isRain, bool isFlash) {
	clearBuffer(playfieldLayer);
	clearBuffer(finalLayer);
	
	if (curTime < hitShakeEndTime && settings.enableKeyEffects) {
		isBoom = true;
		boomIntensity = std::max(boomIntensity, hitShakeIntensity);
	}
	
	if (settings.enableStarfield) {
		for (int y = 0; y < SCREEN_HEIGHT; ++y) {
			for (int x = currentLaneStartX; x < currentLaneStartX + LANE_WIDTH * 4; ++x) {
				if ((x * 17 + (y - curTime / 50) * 31) % 150 == 0) {
					if (x >= 0 && x < SCREEN_WIDTH) {
						playfieldLayer[y][x].Char.AsciiChar = '.';
						playfieldLayer[y][x].Attributes = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
					}
				}
			}
		}
	}
	
	// 绘制轨道底图 (严格进行防越界检测)
	for (int y = settings.hiddenLevel; y <= currentJudgeLineY; ++y) {
		for (int l = 0; l <= 4; ++l) {
			int x = currentLaneStartX + l * LANE_WIDTH;
			if (y >= 2 && x >= 0 && x < SCREEN_WIDTH && y < SCREEN_HEIGHT) {
				playfieldLayer[y][x].Char.AsciiChar = '|';
				playfieldLayer[y][x].Attributes = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
			}
		}
		for (int l = 0; l < 4; ++l) {
			if (prevKeyState[l] && y <= currentJudgeLineY) {
				for (int dx = 1; dx < LANE_WIDTH; ++dx) {
					int x = currentLaneStartX + l * LANE_WIDTH + dx;
					if (y >= 2 && x >= 0 && x < SCREEN_WIDTH && y < SCREEN_HEIGHT) {
						playfieldLayer[y][x].Attributes |= BACKGROUND_INTENSITY;
					}
				}
			}
		}
	}
	
	for (int l = 0; l < 4; ++l) {
		int x = currentLaneStartX + l * LANE_WIDTH + 1;
		if (curTime > laneEffectEndTime[l]) laneColors[l] = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY;
		drawString(playfieldLayer, x, currentJudgeLineY, "=====", laneColors[l]);
	}
	
	for (auto& note : notes) {
		if (note.processed && !note.isBeingHeld) continue;
		
		int y = currentJudgeLineY - static_cast<int>((note.hitTime - curTime) * chartSpeed);
		int endY = y;
		if (note.type == HOLD) {
			endY = currentJudgeLineY - static_cast<int>((note.hitTime + note.duration - curTime) * chartSpeed);
		}
		
		if (endY < SCREEN_HEIGHT && y >= 0) {
			int x = currentLaneStartX + note.lane * LANE_WIDTH + 1;
			WORD colorTap = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
			WORD colorHold = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
			
			if (note.type == TAP && y >= settings.hiddenLevel && y < currentJudgeLineY + 5) {
				drawString(playfieldLayer, x, y, "[###]", colorTap);
			}
			else if (note.type == HOLD) {
				int startDrawY = std::max(settings.hiddenLevel, endY); 
				int endDrawY = std::min(SCREEN_HEIGHT - 1, y);
				for (int drawY = startDrawY; drawY <= endDrawY; ++drawY) {
					if (drawY == startDrawY || drawY == endDrawY) drawString(playfieldLayer, x, drawY, "[===]", colorHold);
					else {
						WORD bodyColor = note.isBeingHeld ? (colorHold | BACKGROUND_GREEN) : colorHold;
						drawString(playfieldLayer, x, drawY, "|||||", bodyColor);
					}
				}
			}
		}
		
		if (note.type == TAP && curTime - note.hitTime > (settings.beginnerMode ? 250 : 180) && !note.processed) {
			note.processed = true; combo = 0; lastJudge = "MISS"; judgeDisplayTimer = 30; statMiss++;
			laneColors[note.lane] = FOREGROUND_INTENSITY; laneEffectEndTime[note.lane] = curTime + 100;
		}
		if (note.type == HOLD && curTime - (note.hitTime + note.duration) > (settings.beginnerMode ? 250 : 180) && !note.processed) {
			note.processed = true; note.isBeingHeld = false; combo = 0; lastJudge = "MISS"; judgeDisplayTimer = 30; statMiss++;
			laneColors[note.lane] = FOREGROUND_INTENSITY; laneEffectEndTime[note.lane] = curTime + 100;
		}
	}
	
	// 更新粒子
	// 【优化】更新粒子位置与生命
	for (auto& p : particles) {
		p.x += p.vx; 
		p.y += p.vy; 
		p.vy += 0.15f; 
		p.life--;
	}
	
	// 【优化】绘制未死亡的粒子
	for (const auto& p : particles) {
		if (p.life > 0) {
			int px = (int)p.x; 
			int py = (int)p.y;
			if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
				playfieldLayer[py][px].Char.AsciiChar = p.symbol;
				playfieldLayer[py][px].Attributes = p.color;
			}
		}
	}
	
	// 【优化】单次遍历高效清理已死亡粒子（从 O(N^2) 降为 O(N)）
	particles.erase(std::remove_if(particles.begin(), particles.end(), 
								   [](const Particle& p) { return p.life <= 0; }), particles.end());
	
	// 处理特效与合成层 (包含Boom特效)
	for (int y = 0; y < SCREEN_HEIGHT; ++y) {
		int xOffset = 0; bool glitch = false;
		if (isBoom) {
			float timeFactor = curTime / 50.0f;
			xOffset = static_cast<int>(sin(y / 3.0f + timeFactor) * 2.0f * boomIntensity);
			glitch = (rand() % 100) < (5 * boomIntensity);
		}
		for (int x = 0; x < SCREEN_WIDTH; ++x) {
			int srcX = x + xOffset;
			if (srcX >= 0 && srcX < SCREEN_WIDTH) {
				finalLayer[y][x] = playfieldLayer[y][srcX];
				if (glitch) { finalLayer[y][x].Char.AsciiChar = "!#$-%^&*"[rand() % 8]; finalLayer[y][x].Attributes = rand() % 16; }
			} else finalLayer[y][x].Char.AsciiChar = ' ';
		}
	}
	
	// 【新增特效：Matrix代码雨】盖在轨道上面
	if (isRain) {
		for (int y = 0; y < SCREEN_HEIGHT; ++y) {
			for (int x = 0; x < SCREEN_WIDTH; ++x) {
				if (rand() % 40 == 0) {
					finalLayer[y][x].Char.AsciiChar = rand() % 94 + 33;
					finalLayer[y][x].Attributes = FOREGROUND_GREEN | (rand() % 2 ? FOREGROUND_INTENSITY : 0);
				}
			}
		}
	}
	
	// Toast UI
	for (const auto& t : toasts) {
		if (curTime >= t.startTime && curTime <= t.endTime) {
			int x = (SCREEN_WIDTH - (int)t.text.length()) / 2;
			drawString(finalLayer, x, t.yOffset, t.text, FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY);
		}
	}
	
	// 绘制顶层 UI
	drawString(finalLayer, 1, 1, " +----------------------+ ", FOREGROUND_BLUE | FOREGROUND_INTENSITY);
	drawString(finalLayer, 1, 2, " | Project: Fall4K      | ", FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	drawString(finalLayer, 1, 3, " | Chart: " + currentChartFile + std::string(14 - std::min((int)currentChartFile.length(), 14), ' ') + "|", FOREGROUND_BLUE | FOREGROUND_INTENSITY);
	drawString(finalLayer, 1, 4, " | Speed: " + std::to_string(chartSpeed).substr(0, 4) + "         |", FOREGROUND_BLUE | FOREGROUND_GREEN);
	drawString(finalLayer, 1, 5, " +----------------------+ ", FOREGROUND_BLUE | FOREGROUND_INTENSITY);
	
	drawString(finalLayer, 2, 7, "TIME: " + std::to_string(curTime) + " / " + std::to_string(chartTotalDuration) + " ms");
	
	int comboY = (curTime - lastHitTime < 80) ? 8 : 9; 
	drawString(finalLayer, 2, comboY, "COMBO: " + std::to_string(combo), FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	drawString(finalLayer, 2, comboY + 1, "MAX  : " + std::to_string(maxCombo), FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	
	// 【新增】实时ACC渲染
	if (settings.showRealTimeAcc) {
		float currentAcc = 0.0f;
		int total = statPerfect + statGreat + statBad + statMiss;
		if (total > 0) currentAcc = ((statPerfect * 100.0f) + (statGreat * 80.0f) + (statBad * 30.0f)) / total;
		char accBuf[32]; sprintf_s(accBuf, "ACC: %05.2f%%", currentAcc);
		drawString(finalLayer, 2, comboY + 3, accBuf, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	}
	
	if (judgeDisplayTimer > 0) {
		WORD judgeColor = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED; std::string judgePad = "";
		if (lastJudge == "PERFECT") { judgeColor = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; judgePad = " PERFECT "; }
		if (lastJudge == "GREAT") { judgeColor = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY; judgePad = " GREAT "; }
		if (lastJudge == "BAD") { judgeColor = FOREGROUND_RED | FOREGROUND_GREEN; judgePad = " BAD "; } 
		if (lastJudge == "MISS") { judgeColor = FOREGROUND_RED | FOREGROUND_INTENSITY; judgePad = " MISS! "; } 
		
		int yOff = (judgeDisplayTimer > 25) ? 1 : 0;
		drawString(finalLayer, SCREEN_WIDTH / 2 - 5, currentJudgeLineY - 4 - yOff, judgePad, judgeColor);
		judgeDisplayTimer--;
	}
	
	// 【新增特效：终极闪烁 / 反色效果】作用于所有图像层
	if (isFlash) {
		for (int y = 0; y < SCREEN_HEIGHT; ++y) {
			for (int x = 0; x < SCREEN_WIDTH; ++x) {
				WORD attr = finalLayer[y][x].Attributes;
				// 将前景色变为背景色，实现色彩反转致盲效果
				WORD fg = attr & 0x0F;
				WORD bg = (attr & 0xF0) >> 4;
				finalLayer[y][x].Attributes = (fg << 4) | bg;
			}
		}
	}
	
	COORD bufferSize = { SCREEN_WIDTH, SCREEN_HEIGHT }; COORD bufferCoord = { 0, 0 }; SMALL_RECT writeRegion = { 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1 };
	WriteConsoleOutput(hConsole, (CHAR_INFO*)finalLayer, bufferSize, bufferCoord, &writeRegion);
}

// ================= 判定系统 =================

void processInput(long long curTime) {
	int pT = settings.beginnerMode ? 100 : 50;
	int gT = settings.beginnerMode ? 200 : 100;
	int bT = settings.beginnerMode ? 250 : 180;
	
	// 【新增】全自动打歌逻辑 (Auto Play)
	if (settings.autoPlay) {
		for (int i = 0; i < 4; ++i) prevKeyState[i] = false; // 清空按压状态，由程序接管
		for (auto& note : notes) {
			if (note.processed) continue;
			long long diff = note.hitTime - curTime;
			
			if (note.type == TAP) {
				if (diff <= 0) { // 精准到达时刻完美击杀
					lastJudge = "PERFECT"; combo++; statPerfect++; maxCombo = std::max(maxCombo, combo);
					judgeDisplayTimer = 30; lastHitTime = curTime;
					laneColors[note.lane] = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; laneEffectEndTime[note.lane] = curTime + 100;
					hitShakeEndTime = curTime + 80; hitShakeIntensity = 0.3f;
					spawnParticles(note.lane, laneColors[note.lane], 8); playTapSound();
					note.processed = true;
				}
			} else if (note.type == HOLD) {
				if (!note.isBeingHeld) {
					if (diff <= 0) { // 自动精准接住
						note.isBeingHeld = true;
						lastJudge = "PERFECT"; judgeDisplayTimer = 10;
						laneColors[note.lane] = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; hitShakeEndTime = curTime + 80; hitShakeIntensity = 0.3f;
						spawnParticles(note.lane, laneColors[note.lane], 5); playTapSound();
					}
				} else {
					prevKeyState[note.lane] = true; // 自动模拟持续按压
					laneEffectEndTime[note.lane] = curTime + 100;
					if (curTime % 3 == 0) spawnParticles(note.lane, FOREGROUND_GREEN | FOREGROUND_INTENSITY, 1, 0.5f);
					if (curTime >= note.hitTime + note.duration) { // 完美松手
						note.isBeingHeld = false; note.processed = true;
						lastJudge = "PERFECT"; combo++; statPerfect++; maxCombo = std::max(maxCombo, combo);
						judgeDisplayTimer = 30; lastHitTime = curTime;
						laneColors[note.lane] = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; laneEffectEndTime[note.lane] = curTime + 100;
						hitShakeEndTime = curTime + 80; hitShakeIntensity = 0.4f; spawnParticles(note.lane, laneColors[note.lane], 12);
					}
				}
			}
		}
	} else {
		// 原有普通手打输入逻辑
		for (int i = 0; i < 4; ++i) {
			bool isKeyDown = (GetAsyncKeyState(KEYS[i]) & 0x8000) != 0;
			bool isKeyPressed = isKeyDown && !prevKeyState[i];
			
			for (auto& note : notes) {
				if (note.lane != i || note.processed) continue;
				
				long long diff = abs(curTime - note.hitTime);
				
				if (note.type == TAP) {
					if (isKeyPressed) {
						if (diff <= pT) { 
							lastJudge = "PERFECT"; combo++; statPerfect++;
							laneColors[i] = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; 
							hitShakeEndTime = curTime + 80; hitShakeIntensity = 0.3f; 
							spawnParticles(i, laneColors[i], 8); playTapSound();
						}
						else if (diff <= gT) { 
							lastJudge = "GREAT"; combo++; statGreat++;
							laneColors[i] = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY; 
							spawnParticles(i, laneColors[i], 4); playTapSound();
						}
						else if (diff <= bT) { 
							lastJudge = "BAD"; combo = 0; statBad++;
							laneColors[i] = FOREGROUND_RED | FOREGROUND_GREEN; playTapSound();
						}
						else continue;
						
						note.processed = true; maxCombo = std::max(maxCombo, combo);
						judgeDisplayTimer = 30; lastHitTime = curTime; laneEffectEndTime[i] = curTime + 100;
						break;
					}
				}
				else if (note.type == HOLD) {
					if (!note.isBeingHeld) {
						if (isKeyDown && diff <= gT) { 
							note.isBeingHeld = true;
							if (diff <= pT) { 
								lastJudge = "PERFECT"; laneColors[i] = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; 
								hitShakeEndTime = curTime + 80; hitShakeIntensity = 0.3f;
							}
							judgeDisplayTimer = 10; spawnParticles(i, laneColors[i], 5); playTapSound();
							break; 
						}
					}
					else { 
						laneEffectEndTime[i] = curTime + 100;
						long long endTime = note.hitTime + note.duration;
						
						if (curTime % 3 == 0) spawnParticles(i, FOREGROUND_GREEN | FOREGROUND_INTENSITY, 1, 0.5f);
						
						if (!isKeyDown) { 
							if (curTime >= endTime - pT) { 
								note.isBeingHeld = false; note.processed = true;
								lastJudge = "PERFECT"; combo++; statPerfect++; judgeDisplayTimer = 30; lastHitTime = curTime;
								laneColors[i] = FOREGROUND_INTENSITY; laneEffectEndTime[i] = curTime + 100;
								spawnParticles(i, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY, 10);
								break;
							}
							else {
								note.isBeingHeld = false; note.processed = true;
								lastJudge = "MISS"; combo = 0; statMiss++; judgeDisplayTimer = 30;
								laneColors[i] = FOREGROUND_INTENSITY; laneEffectEndTime[i] = curTime + 100;
								break;
							}
						}
						else if (curTime >= note.hitTime + note.duration) { 
							note.isBeingHeld = false; note.processed = true;
							lastJudge = "PERFECT"; combo++; statPerfect++; maxCombo = std::max(maxCombo, combo);
							judgeDisplayTimer = 30; lastHitTime = curTime;
							laneColors[i] = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; laneEffectEndTime[i] = curTime + 100;
							hitShakeEndTime = curTime + 80; hitShakeIntensity = 0.4f; spawnParticles(i, laneColors[i], 12);
							break;
						}
						break; 
					}
				}
			}
			prevKeyState[i] = isKeyDown;
		}
	}
}

// ================= 界面模块 =================

// 【特效：滚动数字与解密结算界面】
void renderResults(long long uiTime) {
	clearBuffer(finalLayer);
	long long elapsed = uiTime - globalStateEnterTime;
	float progress = std::min(1.0f, elapsed / 2000.0f); // 2秒解密滚动动画时间
	
	// 背景矩阵干扰
	for(int y=0; y<SCREEN_HEIGHT; ++y) {
		if(rand()%10 == 0) {
			for(int x=0; x<SCREEN_WIDTH; ++x) {
				if(rand()%30 == 0) {
					finalLayer[y][x].Char.AsciiChar = rand()%94 + 33;
					finalLayer[y][x].Attributes = FOREGROUND_GREEN;
				}
			}
		}
	}
	
	int totalNotes = notes.size(); float targetAcc = 0.0f;
	if (totalNotes > 0) {
		float score = (statPerfect * 100.0f) + (statGreat * 80.0f) + (statBad * 30.0f) + (statMiss * 0.0f);
		targetAcc = score / (totalNotes * 100.0f) * 100.0f;
	}
	
	// 动态滚动的数字效果
	float currentAcc = targetAcc * progress;
	int dPerfect = statPerfect * progress;
	int dGreat = statGreat * progress;
	int dBad = statBad * progress;
	int dMiss = statMiss * progress;
	int dMaxCombo = maxCombo * progress;
	
	if (elapsed > 100) drawString(finalLayer, 25, 3, "=======================", FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	if (elapsed > 300) drawString(finalLayer, 27, 4, "S T A G E   C L E A R", FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	if (elapsed > 500) drawString(finalLayer, 25, 5, "=======================", FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	
	if (elapsed > 800) {
		drawString(finalLayer, 35, 10, "CHART: " + currentChartFile, FOREGROUND_BLUE | FOREGROUND_INTENSITY);
		char accBuf[32]; sprintf_s(accBuf, "ACCURACY: %05.2f%%", currentAcc);
		drawString(finalLayer, 35, 12, accBuf, FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		drawString(finalLayer, 35, 14, "MAX COMBO: " + std::to_string(dMaxCombo), FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		
		drawString(finalLayer, 35, 17, "PERFECT : " + std::to_string(dPerfect), FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		drawString(finalLayer, 35, 18, "GREAT   : " + std::to_string(dGreat), FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		drawString(finalLayer, 35, 19, "BAD     : " + std::to_string(dBad), FOREGROUND_RED | FOREGROUND_GREEN);
		drawString(finalLayer, 35, 20, "MISS    : " + std::to_string(dMiss), FOREGROUND_RED | FOREGROUND_INTENSITY);
	}
	
	// 乱码解密 RANK 评级
	if (elapsed > 2000) {
		WORD rankColor = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED; std::string r1, r2, r3, r4, r5;
		if (targetAcc >= 95.0f) { rankColor = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; r1 = "  ____  "; r2 = " / ___| "; r3 = " \\___ \\ "; r4 = "  ___) |"; r5 = " |____/ "; }
		else if (targetAcc >= 90.0f) { rankColor = FOREGROUND_RED | FOREGROUND_INTENSITY; r1 = "    _    "; r2 = "   / \\   "; r3 = "  / _ \\  "; r4 = " / ___ \\ "; r5 = "/_/   \\_\\"; }
		else if (targetAcc >= 80.0f) { rankColor = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY; r1 = " ____  "; r2 = "| __ ) "; r3 = "|  _ \\ "; r4 = "| |_) |"; r5 = "|____/ ";}
		else if (targetAcc >= 70.0f) { rankColor = FOREGROUND_GREEN | FOREGROUND_INTENSITY; r1 = "  ____  "; r2 = " / ___| "; r3 = "| |     "; r4 = "| |___  "; r5 = " \\____| "; }
		else { rankColor = FOREGROUND_INTENSITY; r1 = " ____   "; r2 = "|  _ \\  "; r3 = "| | | | "; r4 = "| |_| | "; r5 = "|____/  "; }
		
		drawString(finalLayer, 10, 10, "RANK:", FOREGROUND_BLUE | FOREGROUND_INTENSITY);
		drawString(finalLayer, 10, 12, r1, rankColor); drawString(finalLayer, 10, 13, r2, rankColor);
		drawString(finalLayer, 10, 14, r3, rankColor); drawString(finalLayer, 10, 15, r4, rankColor); drawString(finalLayer, 10, 16, r5, rankColor);
		
		// 返回提示闪烁
		if ((uiTime / 500) % 2 == 0) drawString(finalLayer, 26, 32, "[ 按 ENTER 键返回主菜单 ]", FOREGROUND_BLUE | FOREGROUND_GREEN);
	} else if (elapsed > 1000) {
		drawString(finalLayer, 10, 10, "DECRYPTING RANK...", FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		for(int i = 0; i < 5; ++i){
			std::string noise = "";
			for(int j = 0; j < 8; ++j) noise += (char)(33 + rand() % 90);
			drawString(finalLayer, 10, 12 + i, noise, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		}
	}
	
	COORD bufferSize = { SCREEN_WIDTH, SCREEN_HEIGHT }; COORD bufferCoord = { 0, 0 }; SMALL_RECT writeRegion = { 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1 };
	WriteConsoleOutput(hConsole, (CHAR_INFO*)finalLayer, bufferSize, bufferCoord, &writeRegion);
}

void processResultsInput() {
	bool currentEnter = (GetAsyncKeyState(VK_RETURN) & 0x8000);
	if (currentEnter && !prevEnterState) {
		currentState = STATE_MENU;
		globalStateEnterTime = GetTickCount64(); // 重置动画计时
	}
	prevEnterState = currentEnter;
}

// 【特效：故障毛刺主菜单与阶梯滑入】
void renderMenu(long long uiTime) {
	clearBuffer(finalLayer);
	long long elapsed = uiTime - globalStateEnterTime;
	
	// 数据雨背景
	for(int y = 0; y < SCREEN_HEIGHT; ++y){
		for(int x = 0; x < SCREEN_WIDTH; ++x){
			if (rand() % 150 == 0) {
				finalLayer[y][x].Char.AsciiChar = rand() % 2 ? '0' : '1';
				finalLayer[y][x].Attributes = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
			} else if (rand() % 50 == 0) {
				finalLayer[y][x].Char.AsciiChar = '.';
				finalLayer[y][x].Attributes = FOREGROUND_GREEN;
			}
		}
	}
	
	// 标题毛刺(Glitch)抖动特效 (5% 概率)
	int glitchX = (rand() % 100 < 5) ? (rand() % 5 - 2) : 0;
	int glitchY = (rand() % 100 < 5) ? (rand() % 3 - 1) : 0;
	drawString(finalLayer, 10 + glitchX, 5 + glitchY, "        ______            __    __   __ __    __ __", FOREGROUND_BLUE | FOREGROUND_INTENSITY);
	drawString(finalLayer, 10 + glitchX, 6 + glitchY, "       / ____/  ____ _   / /   / /  / // /   / //_/", FOREGROUND_BLUE | FOREGROUND_INTENSITY);
	drawString(finalLayer, 10 + glitchX, 7 + glitchY, "      / /_     / __ `/  / /   / /  / // /_  / ,<   ", FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	drawString(finalLayer, 10 + glitchX, 8 + glitchY, "     / __/    / /_/ /  / /   / /  /__  __/ / /| |  ", FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	drawString(finalLayer, 10 + glitchX, 9 + glitchY, "    /_/       \\__,_/  /_/   /_/     /_/   /_/ |_|  ", FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	drawString(finalLayer, 25, 12, "   == 终端4K音游 ==", FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY);
	
	// 菜单项滑入动画与正弦波呼吸指示器
	for (int i = 0; i < MENU_ITEM_COUNT; ++i) {
		int x = SCREEN_WIDTH / 2 - 8; int y = 18 + i * 2;
		
		int slideOffset = std::max(0, 30 - (int)(elapsed / 15) + i * 5); // 阶梯滑入效果
		x += slideOffset;
		if (x >= SCREEN_WIDTH) continue;
		
		if (i == selectedMenuItem) {
			int wave = (sin(uiTime / 100.0) + 1.0) * 2; // 正弦波 0~4
			std::string cursor = std::string(wave, ' ') + "-> ";
			drawString(finalLayer, x - 4 - wave, y, cursor + menuItems[i], FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		} else {
			drawString(finalLayer, x, y, menuItems[i], FOREGROUND_BLUE | FOREGROUND_GREEN);
		}
	}
	
	if (elapsed > 1000) {
		drawString(finalLayer, 22, 30, "用 [W / S] 或 [UP / DOWN] 选择", FOREGROUND_BLUE | FOREGROUND_GREEN);
		drawString(finalLayer, 25, 31, "按下 [ENTER] 开始", FOREGROUND_BLUE | FOREGROUND_GREEN);
		drawString(finalLayer, 27, 35, "(C) 2026 Demo 0.16", FOREGROUND_BLUE | FOREGROUND_INTENSITY);
	}
	
	COORD bufferSize = { SCREEN_WIDTH, SCREEN_HEIGHT }; COORD bufferCoord = { 0, 0 }; SMALL_RECT writeRegion = { 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1 };
	WriteConsoleOutput(hConsole, (CHAR_INFO*)finalLayer, bufferSize, bufferCoord, &writeRegion);
}

void processMenuInput() {
	static bool upPressed = false, downPressed = false; 
	bool currentUp = (GetAsyncKeyState(VK_UP) & 0x8000) || (GetAsyncKeyState('W') & 0x8000);
	bool currentDown = (GetAsyncKeyState(VK_DOWN) & 0x8000) || (GetAsyncKeyState('S') & 0x8000);
	bool currentEnter = (GetAsyncKeyState(VK_RETURN) & 0x8000);
	
	if (currentUp && !upPressed) selectedMenuItem = (selectedMenuItem - 1 + MENU_ITEM_COUNT) % MENU_ITEM_COUNT;
	if (currentDown && !downPressed) selectedMenuItem = (selectedMenuItem + 1) % MENU_ITEM_COUNT;
	
	if (currentEnter && !prevEnterState) {
		if (selectedMenuItem == 0) { chartFiles = scanChartFiles(); selectedChartIndex = 0; currentState = STATE_SELECT_CHART; globalStateEnterTime = GetTickCount64(); }
		else if (selectedMenuItem == 1) { MessageBoxA(NULL, "按键: D, F, J, K\n当蓝键（Tap）到了判定线上时点击\n长条（Hold）是接住的，并不是点击\n本游戏含有异象(画面抖动)\n可以在谱面录制里创作自己的谱面\n在【游戏设置】里可以调整判定、开启新手模式及上隐隐形", "Fall4K - 教程", MB_OK | MB_ICONINFORMATION); }
		else if (selectedMenuItem == 2) { currentState = STATE_REC_SETUP; globalStateEnterTime = GetTickCount64(); }
		else if (selectedMenuItem == 3) { currentState = STATE_SETTINGS; selectedSettingItem = 0; globalStateEnterTime = GetTickCount64(); } 
		else if (selectedMenuItem == 4) { currentState = STATE_EXIT; }
	}
	upPressed = currentUp; downPressed = currentDown; prevEnterState = currentEnter; 
}

// 【修改：下坠扫描线设置界面，支持扩容到8项设置】
void renderSettings(long long uiTime) {
	clearBuffer(finalLayer);
	
	// 背景下落扫描线 (CRT 效果)
	for (int x = 0; x < SCREEN_WIDTH; x += 2) {
		int dropY = (uiTime / (20 + x % 10) + x * 7) % SCREEN_HEIGHT;
		if (dropY >= 0 && dropY < SCREEN_HEIGHT) {
			finalLayer[dropY][x].Char.AsciiChar = '|';
			finalLayer[dropY][x].Attributes = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
		}
	}
	
	// 极客边框
	drawString(finalLayer, 2, 2, "+----------------------------------------------------------------------+", FOREGROUND_GREEN);
	drawString(finalLayer, 2, 37, "+----------------------------------------------------------------------+", FOREGROUND_GREEN);
	
	int titleColor = (rand() % 10 == 0) ? FOREGROUND_GREEN : (FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	drawString(finalLayer, 30, 5, "=== 游戏设置 ===", titleColor);
	drawString(finalLayer, 15, 8, "用 [W/S] 选择, [A/D] 调整, [ESC/ENTER] 保存并返回", FOREGROUND_BLUE | FOREGROUND_GREEN);
	
	std::string itemTexts[8];
	itemTexts[0] = std::string("星空背景特效 : ") + (settings.enableStarfield ? "开 (ON)" : "关 (OFF)");
	itemTexts[1] = std::string("打击粒子特效 : ") + (settings.enableKeyEffects ? "开 (ON)" : "关 (OFF)");
	itemTexts[2] = "隐形(上隐)参数 : " + std::to_string(settings.hiddenLevel) + " 行 (0~34)";
	itemTexts[3] = "音乐延迟补偿 : " + std::string(settings.audioOffset > 0 ? "+" : "") + std::to_string(settings.audioOffset) + " ms";
	itemTexts[4] = std::string("休闲新手模式 : ") + (settings.beginnerMode ? "开 (ON)" : "关 (OFF)");
	itemTexts[5] = std::string("打击音效开关 : ") + (settings.enableTapSound ? "开 (ON)" : "关 (OFF)");
	// 【新增设置项显示】
	itemTexts[6] = std::string("实时准确率(ACC): ") + (settings.showRealTimeAcc ? "开 (ON)" : "关 (OFF)");
	itemTexts[7] = std::string("全自动打歌模式 : ") + (settings.autoPlay ? "开 (ON)" : "关 (OFF)");
	
	for (int i = 0; i < 8; ++i) {
		int x = 25, y = 14 + i * 2;
		if (i == selectedSettingItem) {
			int pulse = (uiTime / 150) % 2;
			std::string brkLeft = pulse ? "[ " : "  ";
			std::string brkRight = pulse ? " ]" : "  ";
			drawString(finalLayer, x - 4, y, brkLeft + "-> " + itemTexts[i] + brkRight, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		} else {
			drawString(finalLayer, x, y, itemTexts[i], FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		}
	}
	
	COORD bufferSize = { SCREEN_WIDTH, SCREEN_HEIGHT }; COORD bufferCoord = { 0, 0 }; SMALL_RECT writeRegion = { 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1 };
	WriteConsoleOutput(hConsole, (CHAR_INFO*)finalLayer, bufferSize, bufferCoord, &writeRegion);
}

void processSettingsInput() {
	static bool upPressed = false, downPressed = false, leftPressed = false, rightPressed = false; 
	bool currentUp = (GetAsyncKeyState(VK_UP) & 0x8000) || (GetAsyncKeyState('W') & 0x8000);
	bool currentDown = (GetAsyncKeyState(VK_DOWN) & 0x8000) || (GetAsyncKeyState('S') & 0x8000);
	bool currentLeft = (GetAsyncKeyState(VK_LEFT) & 0x8000) || (GetAsyncKeyState('A') & 0x8000);
	bool currentRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000) || (GetAsyncKeyState('D') & 0x8000);
	bool currentExit = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000);
	
	// 修改菜单项遍历模数为8
	if (currentUp && !upPressed) selectedSettingItem = (selectedSettingItem - 1 + 8) % 8;
	if (currentDown && !downPressed) selectedSettingItem = (selectedSettingItem + 1) % 8;
	
	if (currentLeft && !leftPressed) {
		if (selectedSettingItem == 0) settings.enableStarfield = !settings.enableStarfield;
		if (selectedSettingItem == 1) settings.enableKeyEffects = !settings.enableKeyEffects;
		if (selectedSettingItem == 2) settings.hiddenLevel = std::max(0, settings.hiddenLevel - 1);
		if (selectedSettingItem == 3) settings.audioOffset = std::max(-300, settings.audioOffset - 10);
		if (selectedSettingItem == 4) settings.beginnerMode = !settings.beginnerMode;
		if (selectedSettingItem == 5) settings.enableTapSound = !settings.enableTapSound;
		if (selectedSettingItem == 6) settings.showRealTimeAcc = !settings.showRealTimeAcc; // 新增控制
		if (selectedSettingItem == 7) settings.autoPlay = !settings.autoPlay;               // 新增控制
	}
	if (currentRight && !rightPressed) {
		if (selectedSettingItem == 0) settings.enableStarfield = !settings.enableStarfield;
		if (selectedSettingItem == 1) settings.enableKeyEffects = !settings.enableKeyEffects;
		if (selectedSettingItem == 2) settings.hiddenLevel = std::min(34, settings.hiddenLevel + 1); 
		if (selectedSettingItem == 3) settings.audioOffset = std::min(300, settings.audioOffset + 10);
		if (selectedSettingItem == 4) settings.beginnerMode = !settings.beginnerMode;
		if (selectedSettingItem == 5) settings.enableTapSound = !settings.enableTapSound;
		if (selectedSettingItem == 6) settings.showRealTimeAcc = !settings.showRealTimeAcc; // 新增控制
		if (selectedSettingItem == 7) settings.autoPlay = !settings.autoPlay;               // 新增控制
	}
	
	if (currentExit && !prevEnterState) {
		saveSettings();
		currentState = STATE_MENU;
		globalStateEnterTime = GetTickCount64();
	}
	
	upPressed = currentUp; downPressed = currentDown; leftPressed = currentLeft; rightPressed = currentRight; prevEnterState = currentExit;
}


void renderChartSelect() {
	clearBuffer(finalLayer);
	drawString(finalLayer, 25, 5, "=== 选择你的谱面 ===", FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	if (chartFiles.empty()) { drawString(finalLayer, 25, 15, "未找到谱面文件 (*.txt)", FOREGROUND_RED | FOREGROUND_INTENSITY); }
	else {
		int maxDisplay = 10, startIdx = 0;
		if (selectedChartIndex >= maxDisplay) startIdx = selectedChartIndex - maxDisplay + 1;
		int endIdx = std::min((int)chartFiles.size(), startIdx + maxDisplay);
		if (startIdx > 0) drawString(finalLayer, 35, 8, "[▲ 更多...]", FOREGROUND_BLUE | FOREGROUND_GREEN);
		
		for (int i = startIdx; i < endIdx; ++i) {
			int x = 20, y = 10 + (i - startIdx) * 2;
			if (i == selectedChartIndex) drawString(finalLayer, x - 4, y, "-> " + chartFiles[i], FOREGROUND_GREEN | FOREGROUND_INTENSITY);
			else drawString(finalLayer, x, y, chartFiles[i], FOREGROUND_BLUE | FOREGROUND_GREEN);
		}
		if (endIdx < (int)chartFiles.size()) drawString(finalLayer, 35, 10 + maxDisplay * 2, "[▼ 更多...]", FOREGROUND_BLUE | FOREGROUND_GREEN);
	}
	drawString(finalLayer, 18, 34, "用 [W/S] 选择, [ENTER] 确认, [ESC] 返回", FOREGROUND_BLUE | FOREGROUND_GREEN);
	
	COORD bufferSize = { SCREEN_WIDTH, SCREEN_HEIGHT }; COORD bufferCoord = { 0, 0 }; SMALL_RECT writeRegion = { 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1 };
	WriteConsoleOutput(hConsole, (CHAR_INFO*)finalLayer, bufferSize, bufferCoord, &writeRegion);
}

void processChartSelectInput() {
	static bool upPressed = false, downPressed = false; 
	bool currentUp = (GetAsyncKeyState(VK_UP) & 0x8000) || (GetAsyncKeyState('W') & 0x8000);
	bool currentDown = (GetAsyncKeyState(VK_DOWN) & 0x8000) || (GetAsyncKeyState('S') & 0x8000);
	bool currentEnter = (GetAsyncKeyState(VK_RETURN) & 0x8000);
	
	if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { currentState = STATE_MENU; globalStateEnterTime = GetTickCount64(); return; }
	
	if (!chartFiles.empty()) {
		if (currentUp && !upPressed) selectedChartIndex = (selectedChartIndex - 1 + (int)chartFiles.size()) % chartFiles.size();
		if (currentDown && !downPressed) selectedChartIndex = (selectedChartIndex + 1) % chartFiles.size();
		
		if (currentEnter && !prevEnterState) {
			currentChartFile = chartFiles[selectedChartIndex];
			loadChart(currentChartFile);
			statPerfect = statGreat = statBad = statMiss = 0; hitShakeEndTime = 0; particles.clear();
			
			startReadyGo();
			currentState = STATE_PLAYING;
			std::string openCmd = "open \"music/" + musicFileName + "\" type mpegvideo alias bgm";
			sendAudioCommand(openCmd); sendAudioCommand("play bgm");
			
			auto start = std::chrono::high_resolution_clock::now();
			gameStartTime = std::chrono::duration_cast<std::chrono::milliseconds>(start.time_since_epoch()).count();
			combo = maxCombo = 0;
		}
	}
	upPressed = currentUp; downPressed = currentDown; prevEnterState = currentEnter; 
}

std::string recMusicName, recChartName; long long recDurationMs = 0;
void setupRecorder() {
	SetConsoleActiveScreenBuffer(GetStdHandle(STD_OUTPUT_HANDLE)); system("cls");
	std::cout << "===== 谱面录制模式 =====\n请输入同目录下音乐名字 (如: music.mp3): "; std::cin >> recMusicName;
	std::cout << "请输入歌曲时长 (毫秒，例如: 120000 代表2分钟): "; std::cin >> recDurationMs;
	std::cout << "请输入新谱面名字 (如: mychart.txt): "; std::cin >> recChartName;
	std::cout << "\n[配置完成]\n按 [回车] 键开始倒计时录制...\n录制期间按 D F J K 打点，按 空格 产生异象，长按 > 180ms 自动识别为Hold，按 ESC 退出保存。\n";
	
	while (!(GetAsyncKeyState(VK_RETURN) & 0x8000)) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
	startReadyGo(); SetConsoleActiveScreenBuffer(hConsole); currentState = STATE_RECORDING; globalStateEnterTime = GetTickCount64();
}

void processRecording() {
	// 保存到 chart 目录
	std::ofstream outfile("chart/" + recChartName);
	outfile << "AUDIO " << recMusicName << "\nTIME " << formatTime(recDurationMs) << "\nSPEED 0.15\n";
	long long startTime = GetTickCount64();
	long long keyStart[4] = { -1, -1, -1, -1 }; long long anomalyStart = -1;
	
	std::string openCmd = "open \"music/" + recMusicName + "\" type mpegvideo alias bgm";
	sendAudioCommand(openCmd); sendAudioCommand("play bgm");
	
	while (true) {
		long long curTime = GetTickCount64() - startTime;
		if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break; 
		
		for (int i = 0; i < 4; ++i) {
			bool isDown = (GetAsyncKeyState(KEYS[i]) & 0x8000) != 0;
			if (isDown) { if (keyStart[i] == -1) keyStart[i] = curTime; }
			else {
				if (keyStart[i] != -1) {
					long long duration = curTime - keyStart[i];
					std::string timeStr = formatTime(keyStart[i]);
					if (duration > 180) outfile << timeStr << " " << (i + 1) << " hold " << duration << "\n";
					else outfile << timeStr << " " << (i + 1) << " tap 0\n";
					keyStart[i] = -1;
				}
			}
		}
		
		bool isSpaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
		if (isSpaceDown) { if (anomalyStart == -1) anomalyStart = curTime; }
		else {
			if (anomalyStart != -1) {
				outfile << formatTime(anomalyStart) << " boom " << formatTime(curTime) << "\n";
				anomalyStart = -1;
			}
		}
		
		clearBuffer(finalLayer);
		drawString(finalLayer, 2, 2, "正在录制: " + recChartName, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		drawString(finalLayer, 2, 4, "已录制时长: " + std::to_string(curTime) + " ms");
		drawString(finalLayer, 2, 6, "按 ESC 结束并保存录制。");
		
		for (int i = 0; i < 4; ++i) {
			if (GetAsyncKeyState(KEYS[i]) & 0x8000) drawString(finalLayer, 5 + i * 10, 10, " [ DOWN ] ", FOREGROUND_RED | FOREGROUND_INTENSITY);
			else drawString(finalLayer, 5 + i * 10, 10, " [  UP  ] ", FOREGROUND_BLUE | FOREGROUND_GREEN);
		}
		
		COORD bufferSize = { SCREEN_WIDTH, SCREEN_HEIGHT }; COORD bufferCoord = { 0, 0 }; SMALL_RECT writeRegion = { 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1 };
		WriteConsoleOutput(hConsole, (CHAR_INFO*)finalLayer, bufferSize, bufferCoord, &writeRegion);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	sendAudioCommand("stop bgm"); sendAudioCommand("close bgm"); outfile.close();
	
	SetConsoleActiveScreenBuffer(GetStdHandle(STD_OUTPUT_HANDLE)); system("cls");
	std::cout << "录制完成并保存至 chart\\" << recChartName << "\n按回车键返回主菜单...\n";
	while (!(GetAsyncKeyState(VK_RETURN) & 0x8000)) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
	SetConsoleActiveScreenBuffer(hConsole); currentState = STATE_MENU; globalStateEnterTime = GetTickCount64();
}

int main() {
	srand((unsigned int)time(NULL)); 
	
	SetConsoleTitleA("Fall4K");
	// 自动创建必需的目录防止崩溃
	CreateDirectoryA("music", NULL);
	CreateDirectoryA("sound", NULL);
	CreateDirectoryA("chart", NULL);
	
	
	loadSettings(); 
	
	
	
	if (!initAudioSystem()) { std::cout << "Warning: Failed to load winmm.dll." << std::endl; std::this_thread::sleep_for(std::chrono::seconds(2)); }
	
	
	hConsole = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
	SetConsoleActiveScreenBuffer(hConsole);
	
	
	CONSOLE_CURSOR_INFO cursorInfo; GetConsoleCursorInfo(hConsole, &cursorInfo); cursorInfo.bVisible = FALSE; SetConsoleCursorInfo(hConsole, &cursorInfo);
	SMALL_RECT windowSize = { 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1 }; SetConsoleWindowInfo(hConsole, TRUE, &windowSize);
	
	
	// 【特效】超酷开场加载动画，按回车可跳过
	playBootAnimation();
	globalStateEnterTime = GetTickCount64();
	
	
	while (currentState != STATE_EXIT) {
		// 【优化】记录帧开始时间，用于精准帧率平滑控制
		auto frame_start = std::chrono::steady_clock::now();
		
		long long uiTime = GetTickCount64(); // 给动画传入实时帧计数
		
		int targetSleep = 0; // 动态目标休眠时间
		
		
		// 原本分散在各处的固定 sleep 均被移到了循环末尾统一做动态平滑控制
		if (currentState == STATE_MENU) { processMenuInput(); renderMenu(uiTime); targetSleep = 20; }
		else if (currentState == STATE_SETTINGS) { processSettingsInput(); renderSettings(uiTime); targetSleep = 20; } 
		else if (currentState == STATE_SELECT_CHART) { processChartSelectInput(); renderChartSelect(); targetSleep = 20; }
		else if (currentState == STATE_REC_SETUP) { setupRecorder(); }
		else if (currentState == STATE_RECORDING) { processRecording(); }
		else if (currentState == STATE_RESULTS) { processResultsInput(); renderResults(uiTime); targetSleep = 20; }
		else if (currentState == STATE_PLAYING) {
			targetSleep = 16; // 游戏内保持 60 FPS (16ms)
			long long curTime = getCurrentTimeMs(); 
			
			if (curTime > chartTotalDuration + 2000) {
				sendAudioCommand("stop bgm"); sendAudioCommand("close bgm");
				currentState = STATE_RESULTS; prevEnterState = true; 
				
				globalStateEnterTime = GetTickCount64(); // 确保结算界面动画重置
			}
			
			
			// 【新增逻辑】实时计算异象动态轨道偏移与特效状态
			float gOffX = 0.0f, gOffY = 0.0f;
			bool isBoom = false, isRain = false, isFlash = false;
			float boomIntensity = 0.0f;
			
			
			for (const auto& a : anomalies) {
				// X 和 Y 移动逻辑 (通过平滑插值实现丝滑偏移)
				if (curTime >= a.startTime) {
					if (a.type == X_SHIFT) {
						if (curTime >= a.endTime) gOffX += a.val;
						else gOffX += a.val * ((float)(curTime - a.startTime) / (a.endTime - a.startTime));
					} else if (a.type == Y_SHIFT) {
						if (curTime >= a.endTime) gOffY += a.val;
						else gOffY += a.val * ((float)(curTime - a.startTime) / (a.endTime - a.startTime));
					}
				}
				// 视觉覆盖效果逻辑 (boom, rain, flash)
				if (curTime >= a.startTime && curTime <= a.endTime) {
					if (a.type == BOOM) {
						isBoom = true; 
						
						boomIntensity = 1.0f - (float)(a.endTime - curTime) / (a.endTime - a.startTime);
					} else if (a.type == RAIN) {
						isRain = true;
					} else if (a.type == FLASH) {
						isFlash = true;
					}
				}
			}
			
			
			// 将计算出的异象偏移实时覆盖到全局绘制基准点
			currentLaneStartX = BASE_LANE_START_X + (int)gOffX;
			currentJudgeLineY = BASE_JUDGE_LINE_Y + (int)gOffY;
			
			
			if (currentState == STATE_PLAYING) { 
				processInput(curTime); 
				renderGame(curTime, isBoom, boomIntensity, isRain, isFlash); 
			}
			
			
			if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
				sendAudioCommand("stop bgm"); sendAudioCommand("close bgm");
				currentState = STATE_MENU; globalStateEnterTime = GetTickCount64();
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
			}
		}
		
		// 【优化】精准帧平滑，减去当前帧的实际耗时，去除微小卡顿
		if (targetSleep > 0) {
			auto frame_end = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start).count();
			if (elapsed < targetSleep) {
				std::this_thread::sleep_for(std::chrono::milliseconds(targetSleep - elapsed));
			}
		}
	}
	return 0;
}
