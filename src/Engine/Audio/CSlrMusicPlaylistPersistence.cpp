#include "CSlrMusicPlaylistPersistence.h"

#include <filesystem>
#include <fstream>

namespace
{
std::filesystem::path NormalizeAbsolutePath(const std::string &path)
{
	if (path.empty())
		return std::filesystem::path();

	std::filesystem::path fsPath(path);
	if (fsPath.is_relative())
		fsPath = std::filesystem::absolute(fsPath);
	return fsPath.lexically_normal();
}

std::string ToGenericString(const std::filesystem::path &path)
{
	return path.lexically_normal().generic_string<char>();
}

bool IsPathWithinRoot(const std::filesystem::path &path, const std::filesystem::path &root)
{
	if (path.empty() || root.empty())
		return false;

	auto rootIt = root.begin();
	auto pathIt = path.begin();
	for (; rootIt != root.end(); ++rootIt, ++pathIt)
	{
		if (pathIt == path.end() || *rootIt != *pathIt)
			return false;
	}

	return true;
}

const char *PathModeToString(CSlrMusicPlaylistPathMode pathMode)
{
	return pathMode == CSlrMusicPlaylistPathMode::ProjectRelative ? "projectRelative" : "absolute";
}

CSlrMusicPlaylistPathMode ParsePathMode(const nlohmann::json &j)
{
	std::string mode = j.value("pathMode", std::string("absolute"));
	if (mode == "projectRelative")
		return CSlrMusicPlaylistPathMode::ProjectRelative;
	return CSlrMusicPlaylistPathMode::Absolute;
}

bool ConvertTrackPathForSave(const std::string &trackPath,
					 const CSlrMusicPlaylistSaveOptions &options,
					 std::string &storedPathOut)
{
	if (options.pathMode == CSlrMusicPlaylistPathMode::Absolute)
	{
		storedPathOut = ToGenericString(NormalizeAbsolutePath(trackPath));
		return true;
	}

	std::filesystem::path rootPath = NormalizeAbsolutePath(options.projectRootPath);
	std::filesystem::path absoluteTrackPath = NormalizeAbsolutePath(trackPath);
	if (rootPath.empty() || absoluteTrackPath.empty())
		return false;
	if (!IsPathWithinRoot(absoluteTrackPath, rootPath))
		return false;

	std::filesystem::path relativePath = absoluteTrackPath.lexically_relative(rootPath);
	if (relativePath.empty())
		return false;

	storedPathOut = ToGenericString(relativePath);
	return !storedPathOut.empty();
}

std::string ResolveTrackPathOnLoad(const std::string &storedPath,
					   CSlrMusicPlaylistPathMode pathMode,
					   const CSlrMusicPlaylistLoadOptions &options)
{
	if (pathMode == CSlrMusicPlaylistPathMode::Absolute)
		return ToGenericString(std::filesystem::path(storedPath));

	if (options.projectRootPath.empty())
		return storedPath;

	std::filesystem::path rootPath = NormalizeAbsolutePath(options.projectRootPath);
	if (rootPath.empty())
		return storedPath;

	return ToGenericString(rootPath / std::filesystem::path(storedPath));
}
}

bool CSlrMusicPlaylistPersistence::SaveToFile(const CSlrMusicPlaylistController &controller,
						 const char *filePath,
						 const CSlrMusicPlaylistSaveOptions &options)
{
	if (filePath == nullptr || filePath[0] == 0)
		return false;

	nlohmann::json j;
	j["version"] = 2;
	j["pathMode"] = PathModeToString(options.pathMode);
	j["settings"]["loopEnabled"] = controller.loopEnabled;
	j["settings"]["randomEnabled"] = controller.randomEnabled;
	j["settings"]["minDistanceBetweenSameTrack"] = controller.minDistanceBetweenSameTrack;
	j["settings"]["playOnAddEnabled"] = controller.playOnAddEnabled;
	j["settings"]["crossfadeEnabled"] = controller.crossfadeEnabled;
	j["settings"]["crossfadeDurationMs"] = controller.crossfadeDurationMs;
	j["settings"]["overlapEnabled"] = controller.overlapEnabled;
	j["settings"]["crossfadeSplineEnabled"] = controller.crossfadeSplineEnabled;
	j["settings"]["crossfadeSplineCp1x"] = controller.crossfadeSplineCp1x;
	j["settings"]["crossfadeSplineCp1y"] = controller.crossfadeSplineCp1y;
	j["settings"]["crossfadeSplineCp2x"] = controller.crossfadeSplineCp2x;
	j["settings"]["crossfadeSplineCp2y"] = controller.crossfadeSplineCp2y;
	j["settings"]["lastOpenFolder"] = controller.lastOpenFolder;
	j["tracks"] = nlohmann::json::array();

	for (const auto &track : controller.tracks)
	{
		std::string storedPath;
		if (!ConvertTrackPathForSave(track.path, options, storedPath))
			return false;

		nlohmann::json jt;
		jt["id"] = track.id;
		jt["path"] = storedPath;
		jt["cueInSample"] = track.cueInSample;
		jt["cueOutSample"] = track.cueOutSample;
		jt["enabled"] = track.enabled;
		j["tracks"].push_back(jt);
	}

	std::ofstream out(filePath, std::ios::trunc);
	if (!out.is_open())
		return false;

	out << j.dump(2);
	return out.good();
}

bool CSlrMusicPlaylistPersistence::LoadFromFile(CSlrMusicPlaylistController &controller,
						 const char *filePath,
						 const CSlrMusicPlaylistLoadOptions &options,
						 CSlrMusicPlaylistDocumentInfo *infoOut)
{
	if (filePath == nullptr || filePath[0] == 0)
		return false;

	std::ifstream in(filePath);
	if (!in.is_open())
		return false;

	nlohmann::json j;
	try
	{
		j = nlohmann::json::parse(in);
	}
	catch (...)
	{
		return false;
	}

	const int version = j.value("version", 1);
	const CSlrMusicPlaylistPathMode pathMode = ParsePathMode(j);

	controller.tracks.clear();
	controller.nextTrackId = 1;

	if (j.contains("settings") && j["settings"].is_object())
	{
		const auto &s = j["settings"];
		controller.loopEnabled = s.value("loopEnabled", false);
		controller.randomEnabled = s.value("randomEnabled", false);
		controller.minDistanceBetweenSameTrack = s.value("minDistanceBetweenSameTrack", 1);
		controller.playOnAddEnabled = s.value("playOnAddEnabled", false);
		controller.crossfadeEnabled = s.value("crossfadeEnabled", false);
		controller.crossfadeDurationMs = s.value("crossfadeDurationMs", 2000);
		controller.overlapEnabled = s.value("overlapEnabled", false);
		controller.crossfadeSplineEnabled = s.value("crossfadeSplineEnabled", false);
		controller.crossfadeSplineCp1x = s.value("crossfadeSplineCp1x", 0.42f);
		controller.crossfadeSplineCp1y = s.value("crossfadeSplineCp1y", 0.0f);
		controller.crossfadeSplineCp2x = s.value("crossfadeSplineCp2x", 0.58f);
		controller.crossfadeSplineCp2y = s.value("crossfadeSplineCp2y", 1.0f);
		controller.lastOpenFolder = s.value("lastOpenFolder", std::string());
	}

	if (j.contains("tracks") && j["tracks"].is_array())
	{
		for (const auto &jt : j["tracks"])
		{
			if (!jt.is_object())
				continue;

			std::string storedPath = jt.value("path", std::string());
			if (storedPath.empty())
				continue;

			CSlrMusicPlaylistTrack track;
			track.id = jt.value("id", controller.nextTrackId);
			track.path = ResolveTrackPathOnLoad(storedPath, pathMode, options);
			track.cueInSample = jt.value<int64_t>("cueInSample", 0LL);
			track.cueOutSample = jt.value<int64_t>("cueOutSample", -1LL);
			track.enabled = jt.value<bool>("enabled", true);
			controller.tracks.push_back(track);
			if (track.id >= controller.nextTrackId)
				controller.nextTrackId = track.id + 1;
		}
	}

	if (controller.minDistanceBetweenSameTrack < 1)
		controller.minDistanceBetweenSameTrack = 1;

	controller.isPlaying = false;
	controller.currentTrackIndex = -1;
	controller.randomRoundPos = -1;
	controller.randomRound.clear();
	controller.playHistoryTrackIds.clear();
	controller.wasAutoStoppedDueToNoEnabled = (controller.CountEnabledTracks() == 0 && !controller.tracks.empty());
	controller.RerandomizeRound();

	if (infoOut != nullptr)
	{
		infoOut->version = version;
		infoOut->pathMode = pathMode;
	}

	return true;
}
