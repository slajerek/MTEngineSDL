class CNpcBrainAgent : public CLlamaPromptAgent {
public:
    using CLlamaPromptAgent::CLlamaPromptAgent;
protected:
    void OnThinkingToken(const std::string &token) override {
        // opcjonalnie: pokaż w debugu że NPC "myśli"
    }
    void OnAnswerToken(const std::string &token) override {
        liveResponseBuffer += token; // live display literka po literce
    }
    void OnStatusChanged(Status s) override {
        if (s == Status::Answering) ShowSpeechBubble(true);
        if (s == Status::Done)      ShowSpeechBubble(false);
    }
    void OnComplete(const MT_LlamaParseResult &result) override {
        ParseNpcDialogue(result.answer);  // wynik gotowy do analizy
        // result.thinking dostępny do logów debugowych
        // result.stopReason do obsługi błędów
    }
};
// Użycie:
agent->SendPrompt("Greet the player!", genParams);
