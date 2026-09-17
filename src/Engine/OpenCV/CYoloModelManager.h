#ifndef _CYoloModelManager_h_
#define _CYoloModelManager_h_

#include "MT_Capabilities.h"

#if MT_ENABLE_YOLO

#include <string>
#include <vector>

// One row of the shipped catalogue.
//
// `licence` is not decoration and is not optional. It is displayed by the
// selector at the moment of choosing, and an engine test asserts that every
// shipped row carries one from the permissive set -- see the note on
// defaultModels in the .cpp for why that is a test rather than a comment.
struct MT_YoloModelDef
{
	std::string id;
	std::string displayName;
	std::string hfRepo;
	std::string hfFilename;
	std::string licence;
	int inputSize = 640;
};

// A model the user pointed at themselves, through Browse.
struct MT_YoloCustomModel
{
	std::string id;
	std::string displayName;
	std::string path;
	int inputSize = 640;
};

// The catalogue, the download URLs, the on-disk paths and the selection.
//
// Deliberately shaped like CLlamaModelManager, down to the HuggingFace URL
// expression, because a host that has wired one should recognise the other.
// It downloads nothing itself: a host drives CFileDownloader with
// GetDownloadUrl() and GetPlannedDownloadPath().
class CYoloModelManager
{
public:
	CYoloModelManager();
	~CYoloModelManager();

	const std::vector<MT_YoloModelDef> &GetDefaultModels() const;

	// "https://huggingface.co/<repo>/resolve/main/<file>", or "" for an
	// unknown id. The same expression CLlamaModelManager uses.
	std::string GetDownloadUrl(const std::string &modelId) const;
	std::string GetPlannedDownloadPath(const std::string &modelId) const;

	// Where the file actually is, once downloaded or browsed to. Empty when
	// the model is not present.
	std::string GetModelPath(const std::string &modelId) const;
	void SetModelPath(const std::string &modelId, const std::string &path);
	bool IsModelPathValid(const std::string &modelId) const;

	int GetInputSize(const std::string &modelId) const;
	std::string GetLicence(const std::string &modelId) const;
	std::string GetDisplayName(const std::string &modelId) const;

	std::string GetSelectedModelId() const;
	void SetSelectedModelId(const std::string &modelId);

	std::string GetModelsDir() const;
	void SetModelFolder(const std::string &dir);

	// Custom models, added through Browse. This is the ONLY route to a model
	// whose licence forbids redistribution in a closed product -- every
	// Ultralytics YOLO is AGPL-3.0 -- so the choice and the obligation are the
	// user's, not this catalogue's.
	const std::vector<MT_YoloCustomModel> &GetCustomModels() const;
	std::string AddCustomModel(const std::string &path, int inputSize);

private:
	// Re-reads the paths AddCustomModel persisted, so a browsed model is still
	// in the list on the next launch. Without it the selection survived and
	// the row that named it did not.
	void LoadCustomModelsFromConfig();

	std::vector<MT_YoloModelDef> defaultModels;
	std::vector<MT_YoloCustomModel> customModels;
};

#endif // MT_ENABLE_YOLO
#endif
