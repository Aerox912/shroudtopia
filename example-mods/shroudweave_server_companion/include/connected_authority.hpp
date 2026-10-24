#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace shroudweave::server {
enum class EditMode { exact_groups, equivalent_permissions, open_connected };
struct PeerAssociation {
    std::uint64_t steam{}, connection{}, world{}, configuration{};
    bool group_verified{};
    std::string group;
    std::uint64_t permissions{};
};
// Implementations must query the actual transport sender's current server
// connection. Unavailable discovery returns nullopt. Payload fields and old
// log authentication lines cannot mint an association.
// Each nonzero connection value is an immutable, never-reused proof. If a
// provider cannot confirm a proof or finds ambiguity, it retires that proof
// before returning nullopt; later recovery requires a new authenticated begin.
struct ConnectedPeerProvider {
    virtual ~ConnectedPeerProvider() = default;
    virtual std::optional<PeerAssociation> current(std::uint64_t transport_sender) = 0;
    // Synchronously invokes operation at most once while connection lifecycle
    // changes are serialized. The callback must be bounded and non-reentrant.
    // Providers without this guarantee fail closed by retaining the default.
    virtual bool with_current(std::uint64_t transport_sender,
        const std::function<void(const PeerAssociation&)>& operation) {
        (void)transport_sender;
        (void)operation;
        return false;
    }
};
struct EditPolicy {
    EditMode mode = EditMode::exact_groups;
    std::uint64_t revision{}, configuration{};
    bool groups_verified{};
    std::uint64_t selected_group_permissions{};
    std::string selected_group;
    bool operator==(const EditPolicy&) const = default;
};
class ConnectedAuthority {
public:
    class Ticket {
        friend class ConnectedAuthority;
        PeerAssociation peer_;
        EditPolicy policy_;
        const ConnectedAuthority* owner_;
        explicit Ticket(PeerAssociation peer, EditPolicy policy, const ConnectedAuthority* owner)
            : peer_(std::move(peer)), policy_(policy), owner_(owner) {}
    public:
        Ticket(const Ticket&) = default;
        std::uint64_t world() const noexcept { return peer_.world; }
    };
    explicit ConnectedAuthority(ConnectedPeerProvider& provider) : provider_(provider) {}
    ConnectedAuthority(const ConnectedAuthority&) = delete;
    ConnectedAuthority& operator=(const ConnectedAuthority&) = delete;
    // Call only with the sender supplied by the shared transport, never an
    // identity parsed from a client message. Tickets stay inside the server.
    std::optional<Ticket> receive(std::uint64_t transport_sender, const EditPolicy& policy);
    bool revalidate(const Ticket& ticket, const EditPolicy& policy);
    bool revalidate(const Ticket& ticket, const PeerAssociation& current,
                    const EditPolicy& policy) const;
    bool with_current(const Ticket& ticket, const EditPolicy& policy,
                      const std::function<void()>& commit) const;
    bool can_read(std::uint64_t transport_sender, std::uint64_t world,
                  std::uint64_t configuration);
private:
    ConnectedPeerProvider& provider_;
    static bool permitted(const PeerAssociation&, const EditPolicy&);
};
}
