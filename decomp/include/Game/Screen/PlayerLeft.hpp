#pragma once

#include "Game/Screen/LayoutActor.hpp"

class CountUpPaneRumbler;
class CounterLayoutAppearer;

class PlayerLeft : public LayoutActor {
public:
    /// @brief Creates a new `PlayerLeft`.
    /// @param pName The pointer to the null-terminated name of the object.
    PlayerLeft(const char* pName);

    virtual void init(const JMapInfoIter& rIter);
    virtual void appear();
    virtual void control();
#ifdef TARGET_PC
    virtual void draw() const;  // left out of the VR diorama with its Counters switch off or a save window up
#endif

    void forceAppear();
    void disappear();
    bool isWait() const;
    void updateCounter();
    bool isValidAppearSituation() const;
    void exeHide();
    void exeAppear();
    void exeWait();
    void exeDisappear();

private:
    /* 0x20 */ s32 mPlayerLeft;
    /* 0x24 */ CounterLayoutAppearer* mLayoutAppearer;
    /* 0x28 */ CountUpPaneRumbler* mPaneRumbler;
    /* 0x2C */ bool mIsForceAppear;
};
