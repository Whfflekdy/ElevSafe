#pragma once

#include <Eigen/Dense>
#include "third_party/kalman-cpp/kalman.hpp"

// 상태: [x, y, z, vx, vy, vz]

inline Eigen::MatrixXd makeA(double dt) {
  Eigen::MatrixXd A = Eigen::MatrixXd::Identity(6, 6);
  A(0, 3) = dt;  // x += vx * dt
  A(1, 4) = dt;  // y += vy * dt
  A(2, 5) = dt;  // z += vz * dt
  return A;
}

inline Eigen::MatrixXd makeQ(double dt, double sigma_a) {
  Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(6, 6);
  double q11 = dt * dt * dt * dt / 4.0;
  double q12 = dt * dt * dt / 2.0;
  double q22 = dt * dt;
  for (int i = 0; i < 3; ++i) {
    Q(i, i)         = q11;
    Q(i, i + 3)     = q12;
    Q(i + 3, i)     = q12;
    Q(i + 3, i + 3) = q22;
  }
  return Q * sigma_a * sigma_a;
}

inline KalmanFilter makeTrack(double dt, const Eigen::Vector3d& first_pos) {
  // 레이더가 위치 3개만 준다고 가정 (3x6)
  Eigen::MatrixXd C = Eigen::MatrixXd::Zero(3, 6);
  C.block(0, 0, 3, 3).setIdentity();

  // 측정 노이즈 (임시값, 녹화 데이터로 튜닝)
  double sigma_r = 0.15;
  Eigen::MatrixXd R = Eigen::MatrixXd::Identity(3, 3) * sigma_r * sigma_r;

  // 초기 오차 공분산 (임시값)
  Eigen::MatrixXd P = Eigen::MatrixXd::Zero(6, 6);
  P.block(0, 0, 3, 3) = Eigen::MatrixXd::Identity(3, 3) * 0.5;
  P.block(3, 3, 3, 3) = Eigen::MatrixXd::Identity(3, 3) * 4.0;

  KalmanFilter kf(dt, makeA(dt), C, makeQ(dt, 1.0), R, P);

  Eigen::VectorXd x0(6);
  x0 << first_pos(0), first_pos(1), first_pos(2), 0, 0, 0;
  kf.init(0.0, x0);
  return kf;
}