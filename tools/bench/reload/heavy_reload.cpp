// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The .cpp script of the body-edit reload benchmark (bench_reload.py). It has the include lines of
// a real BiteSizedWar script and some hundred lines of code. The marker comment with a number is the place
// of the edit (the marker is in GetSlot): the tool changes the number in the body of one method.
#include <Urho3D/Core/Context.h>
#include <Urho3D/Core/Signal.h>
#include <Urho3D/Core/Variant.h>
#include <Urho3D/IO/Deserializer.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/IO/Serializer.h>
#include <Urho3D/Math/MathDefs.h>
#include <Urho3D/Math/Vector3.h>
#include <Urho3D/Replica/BehaviorNetworkObject.h>
#include <Urho3D/Replica/NetworkObject.h>
#include <Urho3D/Scene/Node.h>
#include <Urho3D/Scene/Scene.h>

#include <EASTL/string.h>
#include <EASTL/vector.h>

using namespace Urho3D;

struct HeavyWeaponInfo
{
    int id = 0;
    int magazineSize = 0;
    float fireInterval = 0.1f;
    bool usesAmmo = true;
};

class HeavyHolder : public Object
{
public:
    explicit HeavyHolder(Context* context)
        : Object(context)
    {
        weapons_.push_back({1, 12, 0.25f, true});
        weapons_.push_back({2, 30, 0.08f, true});
        weapons_.push_back({3, 0, 0.6f, false});
    }

    // Return the weapon info of an id, or an info with the id 0.
    HeavyWeaponInfo Find(int id) const
    {
        for (const HeavyWeaponInfo& w : weapons_)
            if (w.id == id)
                return w;
        return HeavyWeaponInfo{};
    }

    bool IsReloading() const { return isReloading_; }

    bool CanFire() const
    {
        const HeavyWeaponInfo info = Find(current_);
        return info.id != 0 && (!info.usesAmmo || (ammo_ > 0 && !isReloading_));
    }

    void SetWeapon(int weapon)
    {
        const HeavyWeaponInfo info = Find(weapon);
        if (info.id == 0)
        {
            URHO3D_LOGWARNING("Unknown weapon {}", weapon);
            return;
        }
        current_ = weapon;
        ammo_ = info.magazineSize;
        isReloading_ = false;
        reloadEnd_ = 0;
        Mark();
    }

    // The slot of a player. The edit of the benchmark changes the number here.
    int GetSlot() const
    {
        return slotBase_ != 0 ? slotBase_ : /*BENCH_EDIT*/ 1;
    }

    bool TryFire(long now)
    {
        if (!CanFire())
            return false;
        const HeavyWeaponInfo info = Find(current_);
        if (info.usesAmmo)
            ammo_--;
        lastShot_ = now;
        Mark();
        return true;
    }

    void StartReload(long now)
    {
        const HeavyWeaponInfo info = Find(current_);
        if (info.id == 0 || !info.usesAmmo || isReloading_)
            return;
        isReloading_ = true;
        reloadEnd_ = now + static_cast<long>(kReloadSeconds * 1.0e9);
        Mark();
    }

    void Update(long now)
    {
        if (isReloading_ && now >= reloadEnd_)
        {
            const HeavyWeaponInfo info = Find(current_);
            ammo_ = info.id != 0 ? info.magazineSize : 0;
            isReloading_ = false;
            Mark();
        }
    }

    // The three bytes of the snapshot: the weapon, the rounds and the reload flag.
    void Write(Serializer& dest) const
    {
        dest.WriteUByte(static_cast<unsigned char>(current_));
        dest.WriteUByte(static_cast<unsigned char>(ammo_ < 0 ? 0 : ammo_ > 255 ? 255 : ammo_));
        dest.WriteUByte(isReloading_ ? 1 : 0);
    }

    bool Read(Deserializer& src)
    {
        if (src.GetSize() - src.GetPosition() < 3)
            return false;
        const int weapon = src.ReadUByte();
        const int ammo = src.ReadUByte();
        const bool reloading = src.ReadUByte() != 0;
        const HeavyWeaponInfo info = Find(weapon);
        if (info.id == 0)
            return false;
        current_ = weapon;
        ammo_ = Min(ammo, info.magazineSize);
        isReloading_ = reloading;
        return true;
    }

    // Add a pending message for the clients.
    void Queue(const ea::string& text)
    {
        pending_.push_back(text);
        if (pending_.size() > 64)
            pending_.erase(pending_.begin());
    }

    int Flush()
    {
        int sent = 0;
        for (unsigned i = 0; i < pending_.size(); i++)
        {
            URHO3D_LOGDEBUG("Send {}", pending_[i]);
            sent++;
        }
        pending_.clear();
        return sent;
    }

    // Attach the holder to a node and place it.
    void Attach(Node* node, const Vector3& offset)
    {
        node_ = node;
        if (node_ != nullptr)
            node_->SetPosition(node_->GetPosition() + offset);
    }

    Vector3 GetMuzzle() const
    {
        if (node_ == nullptr)
            return Vector3::ZERO;
        return node_->GetWorldPosition() + node_->GetWorldDirection() * 0.5f;
    }

    ea::string Describe() const
    {
        const HeavyWeaponInfo info = Find(current_);
        return Format("weapon {} ammo {} reload {} info {}", current_, ammo_, isReloading_, info.id != 0);
    }

    VariantMap Snapshot() const
    {
        VariantMap map;
        map["weapon"] = current_;
        map["ammo"] = ammo_;
        map["reloading"] = isReloading_;
        map["slot"] = GetSlot();
        return map;
    }

    void Restore(const VariantMap& map)
    {
        auto it = map.find("weapon");
        if (it != map.end())
            SetWeapon(it->second.GetInt());
        it = map.find("ammo");
        if (it != map.end())
            ammo_ = it->second.GetInt();
    }

    float Score(float accuracy) const
    {
        float s = 0.0f;
        for (const HeavyWeaponInfo& w : weapons_)
            s += w.magazineSize * accuracy / (w.fireInterval + 0.01f);
        return Clamp(s, 0.0f, 1000.0f);
    }

private:
    void Mark() { revision_++; }

    static constexpr double kReloadSeconds = 1.5;

    ea::vector<HeavyWeaponInfo> weapons_;
    ea::vector<ea::string> pending_;
    WeakPtr<Node> node_;
    int current_ = 1;
    int ammo_ = 0;
    int slotBase_ = 0;
    int revision_ = 0;
    bool isReloading_ = false;
    long reloadEnd_ = 0;
    long lastShot_ = 0;
};
