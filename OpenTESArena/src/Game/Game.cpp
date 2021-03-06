#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "SDL.h"

#include "Game.h"
#include "Options.h"
#include "PlayerInterface.h"
#include "../Assets/CityDataFile.h"
#include "../Interface/Panel.h"
#include "../Media/FontManager.h"
#include "../Media/MusicFile.h"
#include "../Media/MusicName.h"
#include "../Media/TextureManager.h"
#include "../Rendering/Renderer.h"
#include "../Rendering/Surface.h"
#include "../Utilities/Debug.h"
#include "../Utilities/File.h"
#include "../Utilities/Platform.h"
#include "../Utilities/String.h"

#include "components/vfs/manager.hpp"

Game::Game()
{
	DebugMention("Initializing (Platform: " + Platform::getPlatform() + ").");

	// Get the current working directory. This is most relevant for platforms
	// like macOS, where the base path might be in the app's own "Resources" folder.
	this->basePath = Platform::getBasePath();

	// Get the path to the options folder. This is platform-dependent and points inside 
	// the "preferences directory" so it's always writable.
	this->optionsPath = Platform::getOptionsPath();

	// Parse options-default.txt and options-changes.txt (if it exists). Always prefer the
	// default file before the "changes" file.
	this->initOptions(this->basePath, this->optionsPath);

	// Verify that GLOBAL.BSA (the most important Arena file) exists.
	const bool arenaPathIsRelative = File::pathIsRelative(
		this->options.getMisc_ArenaPath());
	const std::string fullArenaPath = [this, arenaPathIsRelative]()
	{
		// Include the base path if the ArenaPath is relative.
		const std::string path = (arenaPathIsRelative ? this->basePath : "") +
			this->options.getMisc_ArenaPath();
		return String::addTrailingSlashIfMissing(path);
	}();

	const std::string globalBsaPath = fullArenaPath + "GLOBAL.BSA";
	DebugAssert(File::exists(globalBsaPath),
		"\"" + this->options.getMisc_ArenaPath() + "\" not a valid ARENA path.");

	// Verify that the floppy version's executable exists. If not, it's probably the CD version,
	// which is not currently supported.
	const std::string exeName = "A.EXE";
	const std::string exePath = fullArenaPath + exeName;
	DebugAssert(File::exists(exePath), exeName + " not found in \"" + fullArenaPath +
		"\". The CD version is not supported. Please use the floppy version.");

	// Initialize virtual file system using the Arena path in the options file.
	VFS::Manager::get().initialize(std::string(
		(arenaPathIsRelative ? this->basePath : "") + this->options.getMisc_ArenaPath()));

	// Initialize the OpenAL Soft audio manager.
	const bool midiPathIsRelative = File::pathIsRelative(this->options.getAudio_MidiConfig());
	const std::string midiPath = (midiPathIsRelative ? this->basePath : "") +
		this->options.getAudio_MidiConfig();

	this->audioManager.init(this->options.getAudio_MusicVolume(),
		this->options.getAudio_SoundVolume(), this->options.getAudio_SoundChannels(),
		this->options.getAudio_SoundResampling(), midiPath);

	// Initialize the SDL renderer and window with the given settings.
	this->renderer.init(this->options.getGraphics_ScreenWidth(),
		this->options.getGraphics_ScreenHeight(), this->options.getGraphics_Fullscreen(),
		this->options.getGraphics_LetterboxMode());

	// Initialize the texture manager.
	this->textureManager.init();

	// Load various miscellaneous assets.
	this->miscAssets.init();

	// Load and set window icon.
	const Surface icon = [this]()
	{
		const std::string iconPath = this->basePath + "data/icon.bmp";
		SDL_Surface *surface = Surface::loadBMP(iconPath, Renderer::DEFAULT_PIXELFORMAT);

		// Treat black as transparent.
		const uint32_t black = SDL_MapRGBA(surface->format, 0, 0, 0, 255);
		SDL_SetColorKey(surface, SDL_TRUE, black);

		return Surface(surface);
	}();

	this->renderer.setWindowIcon(icon.get());

	// Initialize panel and music to default.
	this->panel = Panel::defaultPanel(*this);
	this->setMusic(MusicName::PercIntro);

	// Use a texture as the cursor instead.
	SDL_ShowCursor(SDL_FALSE);

	// Leave some members null for now. The game data is initialized when the player 
	// enters the game world, and the "next panel" is a temporary used by the game
	// to avoid corruption between panel events which change the panel.
	this->gameData = nullptr;
	this->nextPanel = nullptr;
	this->nextSubPanel = nullptr;

	// This keeps the programmer from deleting a sub-panel the same frame it's in use.
	// The pop is delayed until the beginning of the next frame.
	this->requestedSubPanelPop = false;
}

Panel *Game::getActivePanel() const
{
	return (this->subPanels.size() > 0) ?
		this->subPanels.back().get() : this->panel.get();
}

AudioManager &Game::getAudioManager()
{
	return this->audioManager;
}

InputManager &Game::getInputManager()
{
	return this->inputManager;
}

FontManager &Game::getFontManager()
{
	return this->fontManager;
}

bool Game::gameDataIsActive() const
{
	return this->gameData.get() != nullptr;
}

GameData &Game::getGameData() const
{
	// The caller should not request the game data when there is no active session.
	assert(this->gameDataIsActive());

	return *this->gameData.get();
}

Options &Game::getOptions()
{
	return this->options;
}

Renderer &Game::getRenderer()
{
	return this->renderer;
}

TextureManager &Game::getTextureManager()
{
	return this->textureManager;
}

MiscAssets &Game::getMiscAssets()
{
	return this->miscAssets;
}

const FPSCounter &Game::getFPSCounter() const
{
	return this->fpsCounter;
}

void Game::setPanel(std::unique_ptr<Panel> nextPanel)
{
	this->nextPanel = std::move(nextPanel);
}

void Game::pushSubPanel(std::unique_ptr<Panel> nextSubPanel)
{
	this->nextSubPanel = std::move(nextSubPanel);
}

void Game::popSubPanel()
{
	// The active sub-panel must not pop more than one sub-panel, because it may 
	// have unintended side effects for other panels below it.
	DebugAssert(!this->requestedSubPanelPop, "Already scheduled to pop sub-panel.");

	// If there are no sub-panels, then there is only the main panel, and panels 
	// should never have any sub-panels to pop.
	DebugAssert(this->subPanels.size() > 0, "No sub-panels to pop.");

	this->requestedSubPanelPop = true;
}

void Game::setMusic(MusicName name)
{
	const std::string &filename = MusicFile::fromName(name);
	this->audioManager.playMusic(filename);
}

void Game::setGameData(std::unique_ptr<GameData> gameData)
{
	this->gameData = std::move(gameData);
}

void Game::initOptions(const std::string &basePath, const std::string &optionsPath)
{
	// Load the default options first.
	const std::string defaultOptionsPath(basePath + "options/" + Options::DEFAULT_FILENAME);
	this->options.loadDefaults(defaultOptionsPath);

	// Check if the changes options file exists.
	const std::string changesOptionsPath(optionsPath + Options::CHANGES_FILENAME);
	if (!File::exists(changesOptionsPath))
	{
		// Make one. Since the default options object has no changes, the new file will have
		// no key-value pairs.
		DebugMention("Creating options file at \"" + changesOptionsPath + "\".");
		this->options.saveChanges();
	}
	else
	{
		// Read in any key-value pairs in the "changes" options file.
		this->options.loadChanges(changesOptionsPath);
	}
}

void Game::resizeWindow(int width, int height)
{
	// Resize the window, and the 3D renderer if initialized.
	const bool fullGameWindow = this->options.getGraphics_ModernInterface();
	this->renderer.resize(width, height,
		this->options.getGraphics_ResolutionScale(), fullGameWindow);
}

void Game::saveScreenshot(const Surface &surface)
{
	// Get the path + filename to use for the new screenshot.
	const std::string screenshotPath = []()
	{
		const std::string screenshotFolder = Platform::getScreenshotPath();
		const std::string screenshotPrefix("screenshot");
		int imageIndex = 0;

		auto getNextAvailablePath = [&screenshotFolder, &screenshotPrefix, &imageIndex]()
		{
			std::stringstream ss;
			ss << std::setw(3) << std::setfill('0') << imageIndex;
			imageIndex++;
			return screenshotFolder + screenshotPrefix + ss.str() + ".bmp";
		};

		std::string path = getNextAvailablePath();
		while (File::exists(path))
		{
			path = getNextAvailablePath();
		}

		return path;
	}();

	const int status = SDL_SaveBMP(surface.get(), screenshotPath.c_str());

	if (status == 0)
	{
		DebugMention("Screenshot saved to \"" + screenshotPath + "\".");
	}
	else
	{
		DebugCrash("Failed to save screenshot to \"" + screenshotPath + "\": " +
			std::string(SDL_GetError()));
	}
}

void Game::handlePanelChanges()
{
	// If a sub-panel pop was requested, then pop the top of the sub-panel stack.
	if (this->requestedSubPanelPop)
	{
		this->subPanels.pop_back();
		this->requestedSubPanelPop = false;
		
		// Unpause the panel that is now the top-most one.
		const bool paused = false;
		this->getActivePanel()->onPauseChanged(paused);
	}

	// If a new sub-panel was requested, then add it to the stack.
	if (this->nextSubPanel.get() != nullptr)
	{
		// Pause the top-most panel.
		const bool paused = true;
		this->getActivePanel()->onPauseChanged(paused);

		this->subPanels.push_back(std::move(this->nextSubPanel));
	}

	// If a new panel was requested, switch to it. If it will be the active panel 
	// (i.e., there are no sub-panels), then subsequent events will be sent to it.
	if (this->nextPanel.get() != nullptr)
	{
		this->panel = std::move(this->nextPanel);
	}
}

void Game::handleEvents(bool &running)
{
	// Handle events for the current game state.
	SDL_Event e;
	while (SDL_PollEvent(&e) != 0)
	{
		// Application events and window resizes are handled here.
		bool applicationExit = this->inputManager.applicationExit(e);
		bool resized = this->inputManager.windowResized(e);
		bool takeScreenshot = this->inputManager.keyPressed(e, SDLK_PRINTSCREEN);

		if (applicationExit)
		{
			running = false;
		}

		if (resized)
		{
			int width = e.window.data1;
			int height = e.window.data2;
			this->resizeWindow(width, height);

			// Call each panel's resize method. The panels should not be listening for
			// resize events themselves because it's more of an "application event" than
			// a panel event.
			this->panel->resize(width, height);

			for (auto &subPanel : this->subPanels)
			{
				subPanel->resize(width, height);
			}
		}

		if (takeScreenshot)
		{
			// Save a screenshot to the local folder.
			const auto &renderer = this->getRenderer();
			const Surface screenshot = renderer.getScreenshot();
			this->saveScreenshot(screenshot);
		}

		// Panel-specific events are handled by the active panel.
		this->getActivePanel()->handleEvent(e);

		// See if the event requested any changes in active panels.
		this->handlePanelChanges();
	}
}

void Game::tick(double dt)
{
	// Tick the active panel.
	this->getActivePanel()->tick(dt);

	// See if the panel tick requested any changes in active panels.
	this->handlePanelChanges();
}

void Game::render()
{
	// Draw the panel's main content.
	this->panel->render(this->renderer);

	// Draw any sub-panels back to front.
	for (auto &subPanel : this->subPanels)
	{
		subPanel->render(this->renderer);
	}

	// Call the active panel's secondary render method. Secondary render items are those
	// that are hidden on panels below the active one.
	Panel *activePanel = this->getActivePanel();
	activePanel->renderSecondary(this->renderer);

	// Get the active panel's cursor texture and alignment.
	const std::pair<SDL_Texture*, CursorAlignment> cursor = activePanel->getCurrentCursor();

	// Draw cursor if not null. Some panels do not define a cursor (like cinematics), 
	// so their cursor is always null.
	if (cursor.first != nullptr)
	{
		// The panel should not be drawing the cursor themselves. It's done here 
		// just to make sure that the cursor is drawn only once and is always drawn last.
		this->renderer.drawCursor(cursor.first, cursor.second,
			this->inputManager.getMousePosition(), this->options.getGraphics_CursorScale());
	}

	this->renderer.present();
}

void Game::loop()
{
	// Longest allowed frame time in microseconds.
	const std::chrono::duration<int64_t, std::micro> maximumMS(1000000 / Options::MIN_FPS);

	auto thisTime = std::chrono::high_resolution_clock::now();

	// Primary game loop.
	bool running = true;
	while (running)
	{
		const auto lastTime = thisTime;
		thisTime = std::chrono::high_resolution_clock::now();

		// Fastest allowed frame time in microseconds.
		const std::chrono::duration<int64_t, std::micro> minimumMS(
			1000000 / this->options.getGraphics_TargetFPS());

		// Delay the current frame if the previous one was too fast.
		auto frameTime = std::chrono::duration_cast<std::chrono::microseconds>(thisTime - lastTime);
		if (frameTime < minimumMS)
		{
			const auto sleepTime = minimumMS - frameTime;
			std::this_thread::sleep_for(sleepTime);
			thisTime = std::chrono::high_resolution_clock::now();
			frameTime = std::chrono::duration_cast<std::chrono::microseconds>(thisTime - lastTime);
		}

		// Clamp the delta time to at most the maximum frame time.
		const double dt = std::fmin(frameTime.count(), maximumMS.count()) / 1000000.0;

		// Update the input manager's state.
		this->inputManager.update();

		// Update the audio manager, checking for finished sounds.
		this->audioManager.update();

		// Update FPS counter.
		this->fpsCounter.updateFrameTime(dt);

		// Listen for input events.
		try
		{
			this->handleEvents(running);
		}
		catch (const std::exception &e)
		{
			DebugCrash("handleEvents() exception! " + std::string(e.what()));
		}

		// Animate the current game state by delta time.
		try
		{
			this->tick(dt);
		}
		catch (const std::exception &e)
		{
			DebugCrash("tick() exception! " + std::string(e.what()));
		}

		// Draw to the screen.
		try
		{
			this->render();
		}
		catch (const std::exception &e)
		{
			DebugCrash("render() exception! " + std::string(e.what()));
		}
	}

	// At this point, the program has received an exit signal, and is now 
	// quitting peacefully.
	this->options.saveChanges();
}
