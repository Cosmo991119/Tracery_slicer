#ifndef slic3r_BackupSnapshot_hpp_
#define slic3r_BackupSnapshot_hpp_

#include <functional>
#include <memory>

namespace Slic3r {
struct StoreParams;

// Capture on the GUI thread. The worker owns all mutable export inputs; immutable
// model meshes remain shared. Only Backup exports (without UI callbacks) are accepted.
class BackupSnapshot
{
public:
    explicit BackupSnapshot(const StoreParams& params);
    ~BackupSnapshot();
    BackupSnapshot(const BackupSnapshot&) = delete;
    BackupSnapshot& operator=(const BackupSnapshot&) = delete;
    // Serializes behind object-mesh saves in the existing backup worker. Completion
    // runs through run_backup_ui_tasks(), never on the writer thread. Superseded or
    // canceled requests do not acknowledge success.
    static void enqueue(std::shared_ptr<BackupSnapshot> snapshot, std::function<void(bool)> completed);
private:
    struct Data;
    std::unique_ptr<Data> m_data;
    friend class _BBS_Backup_Manager;
};
} // namespace Slic3r
#endif
