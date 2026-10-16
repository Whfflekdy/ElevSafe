#include <iostream>
#include "kalman_track.hpp"

int main() {
  double dt = 0.05;       // 임시값. 실제 레이더 프레임 간격으로 바꿀 것
  double sigma_a = 1.0;

  KalmanFilter kf = makeTrack(dt, Eigen::Vector3d(0.0, 0.0, 1.0));

  for (int i = 1; i <= 20; ++i) {
    kf.predict(dt, makeA(dt), makeQ(dt, sigma_a));

    if (i <= 12) {
      // x 방향으로 초속 0.5m로 움직이는 가짜 측정
      Eigen::VectorXd y(3);
      y << 0.5 * dt * i, 0.0, 1.0;
      kf.correct(y);
    }
    // 13번째 프레임부터는 측정 없음 (predict만 호출)

    std::cout << "frame " << i << ": " << kf.state().transpose() << std::endl;
  }
  return 0;
}