/*
 * Matcherx.cpp
 *
 *  Created on: Jan 12, 2019
 *      Author: sujiwo
 */

#include <Matcher.h>

#include "utilities.h"


using namespace std;
using namespace Eigen;


typedef
	std::map<kpid, std::set<kpid>>
		WhichKpId;


cv::Mat
Matcher::createMatcherMask(
	const KeyFrame &kf1, const KeyFrame &kf2,
	const std::vector<kpid> &kp1List, const std::vector<kpid> &kp2List)
{
	cv::Mat mask (kf2.numOfKeyPoints(), kf1.numOfKeyPoints(), CV_8UC1, 0);

	for (int i=0; i<kp2List.size(); ++i) {
		const kpid &i2 = kp2List[i];
		for (int j=0; j<kp1List.size(); ++j) {
			const kpid &i1 = kp1List[j];
			mask.at<char>(i2, i1) = 1;
		}
	}

	return mask;
}


cv::Mat
Matcher::createMatcherMask(
	const KeyFrame &kf1, const KeyFrame &kf2,
	const WhichKpId &map1to2)
{
	cv::Mat mask (kf2.numOfKeyPoints(), kf1.numOfKeyPoints(), CV_8UC1, 0);

	for (auto &pr: map1to2) {
		const kpid &k1 = pr.first;
		const auto &kp2list = pr.second;
		for (auto &k2: kp2list) {
			mask.at<char>(k2, k1) = 0xff;
		}
	}

	return mask;
}


/*
 * Create an epipolar line in Frame 2 based on Fundamental Matrix F12, using a keypoint from Frame 1
 */
Line2 createEpipolarLine (const Matrix3d &F12, const cv::KeyPoint &kp1)
{
	Line2 epl2;
	epl2.coeffs() = F12 * Vector3d(kp1.pt.x, kp1.pt.y, 1.0);
	return epl2;
}


bool Matcher::isKeypointInEpipolarLine (const Line2 &epl2, const cv::KeyPoint &cvkp2)
{
	Vector2d kp2(cvkp2.pt.x, cvkp2.pt.y);
	auto d = epl2.absDistance(kp2);
	auto lim = 3.84*VMap::mScaleFactors[cvkp2.octave];

	// XXX: Using scale factor makes us more dependent to ORB
	if (d > lim)
		return false;
	else
		return true;
}


void
Matcher::matchForInitialization(
		const KeyFrame &kf1,
		const KeyFrame &kf2,
		std::vector<FeaturePair> &featurePairs,
		cv::Ptr<cv::DescriptorMatcher> matcher)
{
	featurePairs.clear();
	Matrix3d F12 = BaseFrame::FundamentalMatrix(kf1, kf2);

	Line2 L1 = Line2::Through(Vector2d(0,0), Vector2d(kf2.cameraParam.width,0));
	Line2 L2 = Line2::Through(Vector2d(0,0), Vector2d(0,kf2.cameraParam.height));

	set<kpid> kf2targetList;
	WhichKpId kpList1to2;

	for (kpid i1=0; i1<kf1.fKeypoints.size(); i1++) {

		Vector2d keypoint1 (kf1.fKeypoints[i1].pt.x, kf1.fKeypoints[i1].pt.y);

		// Epipolar line in KF2 for this keypoint
		Line2 epl2 = createEpipolarLine(F12, kf1.fKeypoints[i1]);

		// Skip if this line is not intersecting with image rectangle in Frame 2
		Vector2d
			intersect1 = epl2.intersection(L1),
			intersect2 = epl2.intersection(L2);

		if (intersect1.x() < 0 and intersect2.y() > kf2.cameraParam.height)
			continue;
		if (intersect1.x() > kf2.cameraParam.width and intersect2.y() < 0)
			continue;
		if (intersect1.x() > kf2.cameraParam.width and intersect2.y() > kf2.cameraParam.height)
			continue;
		if (intersect1.x() < 0 and intersect2.y() < 0)
			continue;

		kf2targetList.clear();

		for(kpid i2=0; i2<kf2.fKeypoints.size(); ++i2) {

			Vector2d keypoint2 (kf2.fKeypoints[i2].pt.x, kf2.fKeypoints[i2].pt.y);

			if (isKeypointInEpipolarLine(epl2, kf2.fKeypoints[i2]) == false)
				continue;
			kf2targetList.insert(i2);
		}

		if (kf2targetList.size() != 0)
			kpList1to2.insert(make_pair(i1, kf2targetList));
	}

	cv::Mat matcherMask = createMatcherMask(kf1, kf2, kpList1to2);
	vector<cv::DMatch> matchResult;
	matcher->clear();
	matcher->match(kf2.fDescriptors, kf1.fDescriptors, matchResult, matcherMask);

	for (auto &match: matchResult) {
		kpid
			kp1 = match.queryIdx,
			kp2 = match.trainIdx;

		// XXX: Re-Check if kp2 is truly in kp1's epipolar line
		Line2 epl = createEpipolarLine(F12, kf1.fKeypoints[kp1]);

		if (isKeypointInEpipolarLine(epl, kf2.fKeypoints[kp2])==true) {
			FeaturePair pair12 = {kp1, kf1.fKeypoints[kp1].pt, kp2, kf2.fKeypoints[kp2].pt};
			featurePairs.push_back(pair12);
		}
	}
}


void
Matcher::matchAny(
	const BaseFrame &Fr1,
	const BaseFrame &Fr2,
	std::vector<KpPair> &featurePairs,
	cv::Ptr<cv::DescriptorMatcher> matcher,
	TTransform &T12)
{
	// Estimate F
	featurePairs.clear();
	Matrix3d F12 = BaseFrame::FundamentalMatrix(Fr1, Fr2);

	// Establish initial correspondences
	vector<cv::DMatch> initialMatches;
	matcher->match(Fr2.fDescriptors, Fr1.fDescriptors, initialMatches);
	vector<int> inliersMatch;

	// Find outlier/inlier
	for (int ip=0; ip<initialMatches.size(); ++ip) {

		auto dm = initialMatches[ip];

		Line2 line2 = createEpipolarLine(F12, Fr1.fKeypoints[dm.trainIdx]);
		if (isKeypointInEpipolarLine(line2, Fr2.fKeypoints[dm.queryIdx])==true) {
			inliersMatch.push_back(ip);
		}
	}

	// Compute F
	cv::Mat points1(inliersMatch.size(), 2, CV_32F),
			points2(inliersMatch.size(), 2, CV_32F);
	for (int ip=0; ip<inliersMatch.size(); ++ip) {
		cv::DMatch m = initialMatches[inliersMatch[ip]];
		points1.at<float>(ip,0) = Fr1.fKeypoints[m.trainIdx].pt.x;
		points1.at<float>(ip,1) = Fr1.fKeypoints[m.trainIdx].pt.y;
		points2.at<float>(ip,0) = Fr2.fKeypoints[m.queryIdx].pt.x;
		points2.at<float>(ip,1) = Fr2.fKeypoints[m.queryIdx].pt.y;
	}
	cv::Mat Fcv = cv::findFundamentalMat(points1, points2);
	// XXX: Unfinished

	// Let's do matching again

	// Convert F to Essential Matrix E, and compute R & T from Fr1 to Fr2

	// Put valid feature pairs in results
}


cv::Mat
Matcher::drawMatches(
	const BaseFrame &F1,
	const BaseFrame &F2,
	const std::vector<KpPair> &featurePairs,
	DrawMode mode)
{
	cv::Mat result(std::max(F1.height(), F2.height()), F1.width()+F2.width(), F1.image.type());
	F1.image.copyTo( result(cv::Rect(0,0,F1.width(),F1.height())) );
	F2.image.copyTo( result(cv::Rect(F1.width(),0,F2.width(),F2.height())) );

	// XXX: Make list of shifted F2's keypoints
	vector<pair<cv::Point2f, cv::Point2f>> pointPairList(featurePairs.size());
	for (int i=0; i<featurePairs.size(); ++i) {
		cv::Point2f p2 = F2.fKeypoints[i].pt;
		p2.x += F1.width();
		pointPairList[i] = make_pair(
			F1.fKeypoints[i].pt,
			p2);
	}

	const cv::Scalar
		colorBlue(255, 0, 0),
		colorGreen(0, 255, 0),
		colorRed(0, 0, 255);

	// XXX: Unfinished
	if (mode==DrawOpticalFlow) {
		for (auto &pr: pointPairList) {
			cv::circle(result, pr.first, 4, colorBlue);
			cv::circle(result, pr.second, 4, colorRed);
//			cv::Point2f pt1s = pr.first + cv::Vec2f(F1.width(), 0);
//			cv::line(result, pt1s, pr.second, colorGreen);
		}
	}

	else if (mode==DrawSideBySide) {

	}

	else throw runtime_error("Invalid mode");

	return result;
}