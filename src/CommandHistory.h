#pragma once
// CommandHistory.h — lightweight undo/redo command history.
//
// All methods must be called on the MESSAGE THREAD only.
// Commands are pure functions: execute() and undo() must not allocate on
// the audio thread, must not interact with the audio callback, and must be
// exception-safe (noexcept).
//
// USAGE:
//   struct SetTempoCommand : ssbb::Command {
//       AudioEngine& engine;
//       double oldBpm, newBpm;
//       void execute() noexcept override { engine.getTransport().setTempo(newBpm); }
//       void undo()    noexcept override { engine.getTransport().setTempo(oldBpm); }
//   };
//
//   CommandHistory history;
//   history.execute(std::make_unique<SetTempoCommand>(...));
//   history.undo();
//   history.redo();

#include <cstddef>
#include <memory>
#include <vector>

namespace ssbb {

// ---- Command interface --------------------------------------------------

struct Command
{
    virtual ~Command()            = default;
    virtual void execute() noexcept = 0;
    virtual void undo()    noexcept = 0;
};

// ---- CommandHistory -----------------------------------------------------

class CommandHistory
{
public:
    /// Maximum number of undoable steps kept in memory.
    static constexpr std::size_t kDefaultMaxDepth = 256;

    explicit CommandHistory(std::size_t maxDepth = kDefaultMaxDepth)
        : maxDepth_(maxDepth)
    {}

    // Non-copyable (owning unique_ptr vector)
    CommandHistory(const CommandHistory&)            = delete;
    CommandHistory& operator=(const CommandHistory&) = delete;

    /// Execute the command, push it onto the undo stack, and clear the redo
    /// stack (a new action invalidates any previously undone steps).
    /// Oldest entries are discarded when the undo stack exceeds maxDepth_.
    void execute(std::unique_ptr<Command> cmd)
    {
        if (!cmd) return;
        cmd->execute();
        undoStack_.push_back(std::move(cmd));
        redoStack_.clear();

        // Trim oldest entries if we exceeded the depth limit.
        while (undoStack_.size() > maxDepth_)
            undoStack_.erase(undoStack_.begin());
    }

    /// Undo the most recent command and push it onto the redo stack.
    /// No-op if the undo stack is empty.
    void undo()
    {
        if (undoStack_.empty()) return;
        auto& cmd = undoStack_.back();
        cmd->undo();
        redoStack_.push_back(std::move(cmd));
        undoStack_.pop_back();
    }

    /// Redo the most recently undone command.
    /// No-op if the redo stack is empty.
    void redo()
    {
        if (redoStack_.empty()) return;
        auto& cmd = redoStack_.back();
        cmd->execute();
        undoStack_.push_back(std::move(cmd));
        redoStack_.pop_back();
    }

    /// True if there is at least one command that can be undone.
    bool canUndo() const noexcept { return !undoStack_.empty(); }

    /// True if there is at least one command that can be redone.
    bool canRedo() const noexcept { return !redoStack_.empty(); }

    /// Number of steps on the undo stack.
    std::size_t undoDepth() const noexcept { return undoStack_.size(); }

    /// Number of steps on the redo stack.
    std::size_t redoDepth() const noexcept { return redoStack_.size(); }

    /// Clear both stacks (e.g. after loading a new session).
    void clear() noexcept
    {
        undoStack_.clear();
        redoStack_.clear();
    }

private:
    std::size_t                          maxDepth_;
    std::vector<std::unique_ptr<Command>> undoStack_;
    std::vector<std::unique_ptr<Command>> redoStack_;
};

} // namespace ssbb
